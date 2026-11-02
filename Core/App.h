#pragma once
#include "CObject.h"
#include <functional>
#include <future>
#include <memory>
#include <thread>

namespace nfc {

// 앱 뼈대. UI 스레드 + 워커 풀.
// 비동기는 콜백 체인이 아니라 future로 받는다 (C++11).
// 워커 → UI 전달은 PostToUI. UI 객체(Wnd/Control)는 워커에서 직접 건드리지 않는다.
class NFC_API App : public CObject {
public:
    // 실패 시 그때까지 만든 것(GDI+, 알림 창)을 정리하고 false.
    // 기본 스킨이 없으면 정리 후 NfcException을 다시 던진다.
    static bool Init(HINSTANCE h);
    static int Run();
    static void Quit(int code = 0);
    // 워커를 멈추고(남은 작업은 끝까지 실행) GDI+ 캐시와 GDI+를 내린다. 창은 미리 닫을 것.
    static void Shutdown();

    static HINSTANCE Instance();
    static bool IsUiThread();
    static void AssertUiThread();
    static bool IsWorkerPoolRunning();

    // 워커 스레드에서 실행하고 future로 결과 수신. 콜백 없음. (순서 보장 없음)
    // 순서가 필요하면 CObject::Post/RunAsync (객체별 직렬 큐).
    template <typename F>
    static auto RunWorker(F f) -> std::future<decltype(f())> {
        typedef decltype(f()) R;
        std::shared_ptr<std::packaged_task<R()> > task(
            new std::packaged_task<R()>(f));
        std::future<R> fut = task->get_future();
        EnqueueWork([task]() { (*task)(); });
        return fut;
    }

    // UI 스레드에서 실행 (워커 → UI 전달)
    static void PostToUI(std::function<void()> task);

private:
    App();
    static void EnqueueWork(std::function<void()> task);
    static LRESULT CALLBACK NotifyProc(HWND h, UINT msg, WPARAM wp, LPARAM lp);
};

} // namespace nfc
