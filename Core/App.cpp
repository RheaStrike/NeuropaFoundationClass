#include "framework.h"
#include "App.h"
#include "NinePatch.h"
#include "Label.h"
#include "NetworkManager.h"
#include "HttpClient.h"
#include <cassert>
#include <condition_variable>
#include <mutex>
#include <queue>
#include <vector>

namespace nfc {

namespace {
const wchar_t* kNotifyClass = L"NFC_AppNotify";
const UINT kMsgRunTask = WM_APP + 1;

HINSTANCE g_hinst = NULL;
std::thread::id g_uiThread;
HWND g_notify = NULL;
bool g_classRegistered = false;
bool g_initialized = false;

ULONG_PTR g_gdiplus = 0;

std::mutex g_workMutex;
std::condition_variable g_workCv;
std::queue<std::function<void()> > g_work;
std::vector<std::thread> g_workers;
bool g_stop = false;

std::mutex g_uiMutex;
std::queue<std::function<void()> > g_uiTasks;

// 작업 하나 실행. 예외가 워커 스레드를 죽이지 않게 막는다.
// (future로 받는 작업은 packaged_task가 예외를 future에 담으므로 여기까지 오지 않는다.)
void RunGuarded(const std::function<void()>& task) {
    try {
        task();
    } catch (const std::exception& e) {
        OutputDebugStringA("NFC: unhandled exception in task: ");
        OutputDebugStringA(e.what());
        OutputDebugStringA("\n");
    } catch (...) {
        OutputDebugStringA("NFC: unhandled exception in task\n");
    }
}

// 조건 변수로 깨어나는 워커. 폴링·Sleep 없음.
// 멈춤 요청이 와도 큐에 남은 작업은 끝까지 실행하고 나간다.
void WorkerMain() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> l(g_workMutex);
            while (!g_stop && g_work.empty()) g_workCv.wait(l);
            if (g_stop && g_work.empty()) return;
            task = std::move(g_work.front());
            g_work.pop();
        }
        RunGuarded(task);
    }
}

void PushWork(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> l(g_workMutex);
        g_work.push(std::move(task));
    }
    g_workCv.notify_one();
}

void DrainUiTasks() {
    for (;;) {
        std::function<void()> task;
        {
            std::lock_guard<std::mutex> l(g_uiMutex);
            if (g_uiTasks.empty()) return;
            task = std::move(g_uiTasks.front());
            g_uiTasks.pop();
        }
        RunGuarded(task);
    }
}

// 매니페스트 없이 API로 Per-Monitor V2 DPI 인식. 함수가 없는 구 윈도우는 건너뛴다.
void EnableDpiAwareness() {
    typedef BOOL (WINAPI* SetDpiCtxFn)(DPI_AWARENESS_CONTEXT);
    HMODULE user32 = GetModuleHandleW(L"user32.dll");
    if (!user32) return;
    SetDpiCtxFn fn = reinterpret_cast<SetDpiCtxFn>(
        GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
    if (fn) fn(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}
} // namespace

// ---------------------------------------------------------------------------
// 객체별 직렬 큐 (strand)
namespace detail {

// 자체 큐. 비어 있다가 첫 작업이 들어오면 워커 풀에 "비우기" 작업을 하나 넣는다.
// 비우기는 한 워커에서만 돌므로 같은 객체의 작업은 겹치지 않고 넣은 순서대로 돈다.
class Strand : public std::enable_shared_from_this<Strand> {
public:
    Strand() : m_running(false) {}
    void Post(std::function<void()> task) {
        bool start = false;
        {
            std::lock_guard<std::mutex> l(m_mutex);
            m_queue.push(std::move(task));
            if (!m_running) {
                m_running = true;
                start = true;
            }
        }
        if (start) {
            std::shared_ptr<Strand> self = shared_from_this();
            PushWork([self]() { self->Drain(); });
        }
    }
private:
    void Drain() {
        for (;;) {
            std::function<void()> task;
            {
                std::lock_guard<std::mutex> l(m_mutex);
                if (m_queue.empty()) {
                    m_running = false;
                    return;
                }
                task = std::move(m_queue.front());
                m_queue.pop();
            }
            RunGuarded(task);
        }
    }
    std::mutex m_mutex;
    std::queue<std::function<void()> > m_queue;
    bool m_running;
};

} // namespace detail

std::weak_ptr<CObject> CObject::WeakSelf() const {
#if (defined(_MSVC_LANG) && _MSVC_LANG >= 201703L) || __cplusplus >= 201703L
    return std::const_pointer_cast<CObject>(weak_from_this().lock());
#else
    // C++11/14: shared_from_this는 소유되지 않은 객체에서 bad_weak_ptr (MSVC 동작)
    try {
        return std::const_pointer_cast<CObject>(shared_from_this());
    } catch (const std::bad_weak_ptr&) {
        return std::weak_ptr<CObject>();
    }
#endif
}

bool CObject::IsShared() const {
    return !WeakSelf().expired();
}

bool CObject::Post(std::function<void()> task) {
    if (!task) return false;
    std::shared_ptr<CObject> self = WeakSelf().lock();
    if (!self) return false; // 스택 객체 등: 수명을 보장할 수 없으니 거절
    if (!App::IsWorkerPoolRunning()) return false;
    std::call_once(m_strandOnce, [this]() {
        m_strand = std::make_shared<detail::Strand>();
    });
    // self를 붙잡고 큐에 들어간다 → 작업이 끝날 때까지 객체가 살아 있다.
    m_strand->Post([self, task]() { task(); });
    return true;
}

// ---------------------------------------------------------------------------
App::App() {
}

bool App::Init(HINSTANCE h) {
    if (g_initialized) return true;
    g_hinst = h;
    g_uiThread = std::this_thread::get_id();
    EnableDpiAwareness();
    Gdiplus::GdiplusStartupInput si;
    if (Gdiplus::GdiplusStartup(&g_gdiplus, &si, NULL) != Gdiplus::Ok) {
        g_gdiplus = 0;
        return false;
    }
    try {
        NinePatch::Default(); // 기본 스킨 선로드. 없으면 NfcException.
    } catch (...) {
        Shutdown(); // GDI+ 정리 후 다시 던진다
        throw;
    }
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &App::NotifyProc;
    wc.hInstance = h;
    wc.lpszClassName = kNotifyClass;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) {
        Shutdown();
        return false;
    }
    g_classRegistered = true;
    g_notify = CreateWindowExW(0, kNotifyClass, L"", 0, 0, 0, 0, 0,
        HWND_MESSAGE, NULL, h, NULL);
    if (!g_notify) {
        Shutdown();
        return false;
    }
    {
        std::lock_guard<std::mutex> l(g_workMutex);
        g_stop = false;
    }
    unsigned n = std::thread::hardware_concurrency();
    if (n < 2) n = 2;
    for (unsigned i = 0; i < n; ++i) g_workers.push_back(std::thread(&WorkerMain));
    g_initialized = true; // 아래 Shutdown 경로가 정리하도록 먼저 표시
    // 네트워크 전담 매니저(asio) + HTTP(WinHTTP). 실패하면 전부 정리하고 false.
    if (!NetworkManager::Start() || !HttpClient::Start()) {
        Shutdown();
        return false;
    }
    return true;
}

int App::Run() {
    MSG msg;
    msg.wParam = 0;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    return (int)msg.wParam;
}

void App::Quit(int code) {
    PostQuitMessage(code);
}

void App::Shutdown() {
    // Init 도중 실패해도 부를 수 있다. 만든 것만 정리한다.
    // 네트워크 먼저: HTTP 요청 전부 취소 → 소켓 닫기 → io_context stop → join.
    // (네트워크 핸들러가 워커 풀/UI에 넘길 수 있으니 워커보다 먼저)
    HttpClient::Shutdown();
    NetworkManager::Shutdown();
    {
        std::lock_guard<std::mutex> l(g_workMutex);
        g_stop = true;
    }
    g_workCv.notify_all();
    for (size_t i = 0; i < g_workers.size(); ++i) {
        if (g_workers[i].joinable()) g_workers[i].join();
    }
    g_workers.clear();
    if (g_notify) {
        DestroyWindow(g_notify);
        g_notify = NULL;
    }
    if (g_classRegistered) {
        UnregisterClassW(kNotifyClass, g_hinst);
        g_classRegistered = false;
    }
    {
        std::lock_guard<std::mutex> l(g_uiMutex);
        std::queue<std::function<void()> > empty;
        g_uiTasks.swap(empty); // 못 돈 UI 작업은 버린다 (붙잡은 객체 해제)
    }
    // GDI+ 캐시 명시 해제 (GdiplusShutdown 이후 파괴되면 AV). 창은 미리 닫을 것.
    if (g_gdiplus) {
        NinePatch::Shutdown();
        Label::Shutdown();
        Gdiplus::GdiplusShutdown(g_gdiplus);
        g_gdiplus = 0;
    }
    g_initialized = false;
}

HINSTANCE App::Instance() {
    return g_hinst;
}

bool App::IsUiThread() {
    return std::this_thread::get_id() == g_uiThread;
}

void App::AssertUiThread() {
    assert(IsUiThread());
}

bool App::IsWorkerPoolRunning() {
    std::lock_guard<std::mutex> l(g_workMutex);
    return !g_stop && !g_workers.empty();
}

void App::EnqueueWork(std::function<void()> task) {
    PushWork(std::move(task));
}

void App::PostToUI(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> l(g_uiMutex);
        g_uiTasks.push(std::move(task));
    }
    if (g_notify) PostMessage(g_notify, kMsgRunTask, 0, 0);
}

LRESULT CALLBACK App::NotifyProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == kMsgRunTask) {
        DrainUiTasks();
        return 0;
    }
    return DefWindowProc(h, msg, wp, lp);
}

} // namespace nfc
