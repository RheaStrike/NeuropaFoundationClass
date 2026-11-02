#include "framework.h"
#include "Wnd.h"
#include "App.h"
#include <cassert>
#include <map>

namespace nfc {

namespace {
const wchar_t* kClassName = L"NFC_Wnd";
// Invalidate 합치기용 내부 메시지 (사용자 WM_APP 영역과 겹치지 않게 끝 쪽)
const UINT kMsgRender = WM_APP + 0x3F00;
std::map<HWND, Wnd*> g_map;
bool g_registered = false;

// 실행 파일의 서브시스템으로 CUI 여부를 판단한다.
// (VS 드롭다운의 GUI/CUI 선택 = 링커 SubSystem. 콘솔 유무가 아니다.)
bool IsConsoleSubsystem() {
    HMODULE exe = GetModuleHandleW(NULL);
    if (!exe) return false;
    const BYTE* base = reinterpret_cast<const BYTE*>(exe);
    const IMAGE_DOS_HEADER* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) return false;
    const IMAGE_NT_HEADERS* nt =
        reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) return false;
    return nt->OptionalHeader.Subsystem == IMAGE_SUBSYSTEM_WINDOWS_CUI;
}
} // namespace

Wnd::Wnd() : m_hwnd(NULL), m_w(0), m_h(0),
        m_renderPending(false), m_trackingMouse(false) {
}

Wnd::~Wnd() {
    // 여기서는 파생 부분이 이미 파괴됐다. 가상 콜백이 불리지 않도록
    // 먼저 맵/USERDATA에서 떼어 낸 뒤 파괴한다 (이후 메시지는 DefWindowProc).
    if (m_hwnd) {
        HWND h = m_hwnd;
        m_hwnd = NULL;
        g_map.erase(h);
        SetWindowLongPtr(h, GWLP_USERDATA, 0);
        DestroyWindow(h);
    }
}

bool Wnd::EnsureClass(HINSTANCE h) {
    if (g_registered) return true;
    WNDCLASSEXW wc;
    ZeroMemory(&wc, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = &Wnd::WndProc;
    wc.hInstance = h;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    wc.lpszClassName = kClassName;
    if (!RegisterClassExW(&wc) && GetLastError() != ERROR_CLASS_ALREADY_EXISTS) return false;
    g_registered = true;
    return true;
}

bool Wnd::Create(Wnd* parent, const wchar_t* title, int x, int y, int w, int h,
        DWORD style, DWORD exStyle) {
    const bool cui = IsConsoleSubsystem();
    assert(!cui && "CUI에서는 Wnd를 만들 수 없다");
    if (cui) return false;
    App::AssertUiThread();
    HINSTANCE hi = App::Instance();
    if (!EnsureClass(hi)) return false;
    m_w = w;
    m_h = h;
    HWND hp = parent ? parent->m_hwnd : NULL;
    m_hwnd = CreateWindowExW(exStyle, kClassName, title ? title : L"", style,
        x, y, w, h, hp, NULL, hi, this);
    return m_hwnd != NULL;
}

Wnd* Wnd::FromHwnd(HWND h) {
    std::map<HWND, Wnd*>::iterator it = g_map.find(h);
    return it != g_map.end() ? it->second : NULL;
}

LRESULT CALLBACK Wnd::WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_NCCREATE) {
        CREATESTRUCT* cs = reinterpret_cast<CREATESTRUCT*>(lp);
        Wnd* self = reinterpret_cast<Wnd*>(cs->lpCreateParams);
        self->m_hwnd = h;
        SetWindowLongPtr(h, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(self));
        g_map[h] = self;
    }
    Wnd* self = FromHwnd(h);
    if (self) {
        LRESULT r = self->HandleMessage(msg, wp, lp);
        if (msg == WM_NCDESTROY) {
            g_map.erase(h);
            self->m_hwnd = NULL;
            self->m_renderPending = false;
            self->m_trackingMouse = false;
        }
        return r;
    }
    return DefWindowProc(h, msg, wp, lp);
}

LRESULT Wnd::HandleMessage(UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE:
        OnCreate();
        return 0;
    case WM_DESTROY:
        OnDestroy();
        return 0;
    case WM_ERASEBKGND:
        return 1;
    case WM_PAINT: {
        PAINTSTRUCT ps;
        BeginPaint(m_hwnd, &ps);
        EndPaint(m_hwnd, &ps);
        RenderLayered();
        return 0;
    }
    case WM_SIZE: {
        // 위치만 바뀌면 WM_SIZE가 안 오지만, 같은 크기로 오는 경우도 다시 그리지 않는다
        const int nw = LOWORD(lp), nh = HIWORD(lp);
        const bool changed = nw != m_w || nh != m_h || !m_renderer.get();
        m_w = nw;
        m_h = nh;
        OnSize(m_w, m_h);
        if (changed) RenderLayered(); // 크기 변경은 즉시 (표면도 이때만 재생성)
        return 0;
    }
    case WM_LBUTTONDOWN:
        if (OnLeftDown(GET_X_LPARAM(lp), GET_Y_LPARAM(lp))) {
            SetCapture(m_hwnd); // 컨트롤일 때만 캡처 (창 밖에서 떼어도 OnLeftUp이 오게)
        } else {
            // 빈 곳 = 드래그. 캡처를 잡지 않고(혹시 있으면 풀고) 바로 시스템 이동 루프로.
            // PostMessage로 미루면 첫 마우스 이동이 루프 밖에서 처리돼 시작이 멈칫한다.
            // WM_NCLBUTTONDOWN의 lParam은 화면 좌표.
            if (GetCapture() == m_hwnd) ReleaseCapture();
            POINT pt = { GET_X_LPARAM(lp), GET_Y_LPARAM(lp) };
            ClientToScreen(m_hwnd, &pt);
            SendMessage(m_hwnd, WM_NCLBUTTONDOWN, HTCAPTION, MAKELPARAM(pt.x, pt.y));
        }
        return 0;
    case WM_LBUTTONUP:
        OnLeftUp(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        if (GetCapture() == m_hwnd) ReleaseCapture();
        return 0;
    case WM_CAPTURECHANGED:
        if (reinterpret_cast<HWND>(lp) != m_hwnd) OnCaptureLost();
        return 0;
    case WM_MOUSEMOVE:
        if (!m_trackingMouse) {
            TRACKMOUSEEVENT tme;
            ZeroMemory(&tme, sizeof(tme));
            tme.cbSize = sizeof(tme);
            tme.dwFlags = TME_LEAVE;
            tme.hwndTrack = m_hwnd;
            if (TrackMouseEvent(&tme)) m_trackingMouse = true;
        }
        OnMouseMove(GET_X_LPARAM(lp), GET_Y_LPARAM(lp));
        return 0;
    case WM_MOUSELEAVE:
        m_trackingMouse = false;
        OnMouseLeave();
        return 0;
    case WM_KEYDOWN:
        OnKeyDown((unsigned)wp);
        return 0;
    case WM_CHAR:
        OnChar((wchar_t)wp);
        return 0;
    default:
        if (msg == kMsgRender) {
            m_renderPending = false;
            RenderLayered();
            return 0;
        }
        break;
    }
    return DefWindowProc(m_hwnd, msg, wp, lp);
}

void Wnd::OnPaintLayered(Renderer& r, int w, int h) {
    (void)r; (void)w; (void)h;
}

void Wnd::OnCreate() {
}

void Wnd::OnDestroy() {
}

void Wnd::OnSize(int w, int h) {
    (void)w; (void)h;
}

bool Wnd::OnLeftDown(int x, int y) {
    (void)x; (void)y;
    return false;
}

void Wnd::OnLeftUp(int x, int y) {
    (void)x; (void)y;
}

void Wnd::OnMouseMove(int x, int y) {
    (void)x; (void)y;
}

void Wnd::OnMouseLeave() {
}

void Wnd::OnCaptureLost() {
}

void Wnd::OnKeyDown(unsigned vk) {
    (void)vk;
}

void Wnd::OnChar(wchar_t ch) {
    (void)ch;
}

void Wnd::RenderLayered() {
    if (!m_hwnd || m_w <= 0 || m_h <= 0) return;
    App::AssertUiThread();
    if (!m_renderer.get()) {
        // D2D/D3D는 아직 없다 (Renderer::Create가 NULL). 지금은 GDI+ 고정.
        m_renderer = Renderer::Create(RENDER_GDIPLUS);
        if (!m_renderer.get()) return;
    }
    if (!m_renderer->BeginFrame(m_w, m_h)) return;
    OnPaintLayered(*m_renderer, m_w, m_h);
    m_renderer->EndFrame();
    m_renderer->PresentLayered(m_hwnd);
}

void Wnd::Show(int cmd) {
    App::AssertUiThread();
    if (m_hwnd) ShowWindow(m_hwnd, cmd);
}

void Wnd::Hide() {
    Show(SW_HIDE);
}

void Wnd::Close() {
    App::AssertUiThread();
    if (m_hwnd) DestroyWindow(m_hwnd);
}

void Wnd::Invalidate() {
    App::AssertUiThread();
    if (!m_hwnd || m_renderPending) return;
    if (PostMessage(m_hwnd, kMsgRender, 0, 0)) m_renderPending = true;
}

void Wnd::Move(int x, int y) {
    App::AssertUiThread();
    if (m_hwnd) SetWindowPos(m_hwnd, NULL, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER);
}

void Wnd::Resize(int w, int h) {
    App::AssertUiThread();
    if (m_hwnd) SetWindowPos(m_hwnd, NULL, 0, 0, w, h, SWP_NOMOVE | SWP_NOZORDER);
}

void Wnd::CenterWindow() {
    App::AssertUiThread();
    if (!m_hwnd) return;
    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);
    Move((sx - m_w) / 2, (sy - m_h) / 2);
}

void Wnd::SetText(const std::wstring& t) {
    App::AssertUiThread();
    if (m_hwnd) SetWindowText(m_hwnd, t.c_str());
}

} // namespace nfc
