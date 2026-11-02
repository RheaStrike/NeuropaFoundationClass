#pragma once
#include "NetworkManager.h"
#include "Channel.h"
#include <memory>
#include <mutex>
#include <string>

// 공개 헤더. asio를 노출하지 않는다 (pimpl).

namespace nfc {

enum NetEventType { NET_CONNECTED = 0, NET_DISCONNECTED = 1, NET_ERROR = 2 };

struct NetEvent {
    NetEvent() : type(NET_ERROR), code(0) {}
    NetEventType type;
    int code;            // 오류 코드 (asio/WinSock). 0 = 없음
    std::string message; // ASCII 설명
};

// TCP 클라이언트 소켓 (shared_ptr 소유). 만들면 자동으로 NetworkManager에 등록된다
// (Mode::Delegate = 매니저 io_context, Mode::Own = 자기 io_context+스레드)
// → 컨트롤이 직접 만들어도 매니저의 io_context를 쓰고, Shutdown 때 함께 정리된다.
// * 한 소켓의 I/O 핸들러는 자기 strand에서 직렬로 돈다.
// * 기본 전달 경로 = 소유자 직접: SetHandlers(owner, onReceive, onEvent).
//   UI 스레드(PostToUI)에서 소유자 자신의 핸들러가 불린다. 소유자는 weak로 잡으며,
//   소유자가 사라졌으면 데이터는 조용히 버리고 소켓을 닫는다. 부모를 거치지 않는다.
// * 선택: 닫혀 있는 창을 위해 받아 둘 필요가 있을 때만 SetChannels로 Channel에도 Push.
// * Close/Send/Connect는 어느 스레드에서든 호출 가능. Close는 몇 번 불러도 안전.
// * 마지막 참조가 어느 스레드에서 풀려도 안전 (소멸자는 닫기만 예약한다).
class NFC_API Socket : public CObject {
public:
    struct Impl;

    // 매니저가 멈춰 있으면 NULL
    static std::shared_ptr<Socket> Create(Mode mode = Mode::Delegate);
    virtual ~Socket();

    // UI 스레드. 소유자 직접 전달 핸들러 등록 (소유자는 weak로 잡는다)
    template <typename O>
    void SetHandlers(const std::shared_ptr<O>& owner,
            std::function<void(O&, const Bytes&)> onReceive,
            std::function<void(O&, const NetEvent&)> onEvent) {
        App::AssertUiThread();
        std::weak_ptr<O> w = owner;
        m_onReceive = [w, onReceive](const Bytes& b) {
            std::shared_ptr<O> o = w.lock();
            if (!o) return false;
            if (onReceive) onReceive(*o, b);
            return true;
        };
        m_onEvent = [w, onEvent](const NetEvent& e) {
            std::shared_ptr<O> o = w.lock();
            if (!o) return false;
            if (onEvent) onEvent(*o, e);
            return true;
        };
    }
    void ClearHandlers(); // UI 스레드

    // 선택: 채널에도 Push (I/O 스레드에서 Push, 전달은 채널 규칙대로 UI 스레드)
    void SetChannels(const std::shared_ptr<Channel<Bytes> >& data,
        const std::shared_ptr<Channel<NetEvent> >& events);

    // 선택: I/O 스레드(소켓 strand)에서 받은 데이터를 먼저 처리 (서버/프로토콜용, UI 금지).
    // 여기서 예외가 나면 이 소켓만 오류 이벤트 후 닫힌다.
    void SetIoReceiveHandler(std::function<void(Socket&, const Bytes&)> h);

    bool Connect(const std::string& host, unsigned short port); // 비동기. 결과는 이벤트로
    bool Send(const Bytes& data);           // 전송 큐 한도 초과·닫힘이면 false
    bool Send(const std::string& data);
    void SetMaxSendQueue(size_t bytes);     // 기본 1MB
    size_t QueuedBytes() const;
    void CloseWhenSent();                   // 큐를 다 보낸 뒤 닫기
    void Close();                           // 즉시 닫기 (idempotent, 임의 스레드)
    bool IsClosed() const;
    Mode GetMode() const { return m_mode; }

    // --- 내부용 (Network.cpp) ---
    void PostData(const Bytes& data);       // I/O 스레드 → 채널 + UI
    void PostEvent(const NetEvent& ev);
    static std::shared_ptr<Socket> Wrap(const std::shared_ptr<Impl>& impl);

private:
    explicit Socket(Mode mode) : m_mode(mode) {}
    void DeliverData(const Bytes& data);    // UI 스레드
    void DeliverEvent(const NetEvent& ev);

    Mode m_mode;
    std::shared_ptr<Impl> m_impl;
    std::function<bool(const Bytes&)> m_onReceive;   // UI 스레드 전용
    std::function<bool(const NetEvent&)> m_onEvent;  // UI 스레드 전용
    std::mutex m_chMutex;
    std::shared_ptr<Channel<Bytes> > m_dataCh;
    std::shared_ptr<Channel<NetEvent> > m_eventCh;
};

// 최소 TCP 수락 소켓 (테스트/데모용: accept 후 Socket으로 넘긴다).
// 수락 핸들러는 I/O 스레드에서 불린다. 수락된 소켓은 리스너가 닫힐 때까지 리스너가 쥔다.
class NFC_API TcpListener : public CObject {
public:
    struct Impl;
    // port 0 = 임의 포트. loopbackOnly면 127.0.0.1에만 묶는다. 실패/매니저 정지 시 NULL
    static std::shared_ptr<TcpListener> Create(unsigned short port = 0, bool loopbackOnly = true,
        std::function<void(const std::shared_ptr<Socket>&)> onAccept = nullptr);
    virtual ~TcpListener();
    unsigned short Port() const;
    void Close(); // 수락 중단 + 수락된 소켓 전부 닫기 (idempotent)

private:
    TcpListener() : m_port(0) {}
    std::shared_ptr<Impl> m_impl;
    unsigned short m_port;
};

} // namespace nfc
