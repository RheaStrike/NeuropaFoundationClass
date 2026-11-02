#pragma once
#include "NFC_API.h"
#include <exception>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>

namespace nfc {

// 스킨 없음 등 치명적 초기화 실패. 메시지는 ASCII만 쓴다.
class NFC_API NfcException : public std::runtime_error {
public:
    explicit NfcException(const std::string& msg) : std::runtime_error(msg) {}
};

namespace detail { class Strand; } // 객체별 직렬 작업 큐 (App.cpp)

// 모든 NFC 객체의 뿌리.
// * 수명: shared_ptr로 소유하면 "파괴는 잊어라"(C#처럼). 비동기 작업은 객체의
//   shared_ptr를 붙잡고 돌므로, 작업이 큐에 있거나 도는 동안 객체는 죽지 않는다.
// * 스레드: 폴링·임의 뮤텍스 대신 이벤트 구동. Post한 작업은 객체별 직렬 큐(strand)에
//   들어가 App 워커 풀에서 한 번에 하나씩, 넣은 순서대로 돈다 (조건 변수로 깨움, Sleep 없음).
//   (asio는 네트워크 전담 NetworkManager만 쓴다. 이 큐는 항상 자체 구현.)
// * 스택 객체(Dialog dlg; NinePatch np; 등)도 그대로 쓸 수 있다. 단 Post/RunAsync는
//   shared_ptr로 소유된 객체에서만 동작한다 (아니면 Post는 false, RunAsync는 예외를 담은 future).
// * Wnd/Control/Dialog는 UI 스레드 전용 (App::AssertUiThread). strand를 쓰지 않는다.
//   워커 결과는 App::PostToUI로 UI 스레드에 돌려준다.
class NFC_API CObject : public std::enable_shared_from_this<CObject> {
public:
    CObject() {}
    virtual ~CObject() {}

    CObject(const CObject&) = delete;
    CObject& operator=(const CObject&) = delete;

    // shared_ptr로 소유되고 있는가 (Post/RunAsync 가능 여부)
    bool IsShared() const;
    // 소유 중이면 그 weak_ptr, 아니면 빈 weak_ptr
    std::weak_ptr<CObject> WeakSelf() const;

    // 객체별 직렬 큐에 작업 추가. 워커 풀에서 순서대로 하나씩 실행된다.
    // shared_ptr 소유가 아니거나 워커 풀이 멈췄으면 false (작업은 버려진다).
    bool Post(std::function<void()> task);

    // Post + future. 결과(또는 예외)를 future로 받는다. 콜백 없음.
    // 거절되면 NfcException을 담은 future를 돌려준다 (get()에서 던짐).
    template <typename F>
    auto RunAsync(F f) -> std::future<decltype(f())> {
        typedef decltype(f()) R;
        std::shared_ptr<std::packaged_task<R()> > task(
            new std::packaged_task<R()>(f));
        std::future<R> fut = task->get_future();
        if (!Post([task]() { (*task)(); })) {
            std::promise<R> p;
            p.set_exception(std::make_exception_ptr(NfcException(
                "RunAsync rejected: object not owned by shared_ptr or pool stopped")));
            return p.get_future();
        }
        return fut;
    }

private:
    std::once_flag m_strandOnce;
    std::shared_ptr<detail::Strand> m_strand; // 첫 Post 때 만든다
};

} // namespace nfc
