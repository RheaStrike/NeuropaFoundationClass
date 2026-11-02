#pragma once
// 데모 창 클래스들: 메인 창(DemoDialog), 수신 창(ReceiveDialog), 네트워크 에코(EchoDemo).
// 컨트롤은 shared_ptr 멤버로 소유하고, 정리는 스마트 포인터와 소멸자에 맡긴다.
#include "framework.h"
#include "Dialog.h"
#include "Label.h"
#include "Button.h"
#include "Channel.h"
#include "Socket.h"
#include <atomic>
#include <memory>
#include <thread>

namespace demo {

// 소켓 예제 (loopback 에코, 외부 접속 없음).
// 서버는 받은 그대로 돌려보내고, 클라이언트 수신은 소유자 라벨이 직접 받는다.
// 워커 스레드가 1초마다 "메시지 N"을 보내고, 받은 것은 채널(최대 100개)에도 쌓인다.
class EchoDemo {
public:
    typedef nfc::Channel<nfc::Bytes> Feed;

    EchoDemo();
    ~EchoDemo(); // Stop()과 같다

    // owner: 에코 결과를 표시할 라벨 (소켓 핸들러의 소유자)
    void Start(const std::shared_ptr<nfc::Label>& owner);
    // 송신 워커 정지 + 소켓/리스너 닫기. 여러 번 불러도 안전.
    void Stop();
    // 수신 창이 구독하는 채널 (Start 이후 유효)
    const std::shared_ptr<Feed>& GetFeed() const { return m_feed; }

private:
    EchoDemo(const EchoDemo&) = delete;
    EchoDemo& operator=(const EchoDemo&) = delete;

    std::shared_ptr<nfc::TcpListener> m_listener; // 에코 서버
    std::shared_ptr<nfc::Socket> m_socket;        // 클라이언트
    std::shared_ptr<Feed> m_feed;                 // 수신 창용 채널
    std::shared_ptr<int> m_echoCount;             // 핸들러가 공유하는 에코 횟수
    std::atomic<bool> m_stop;
    std::thread m_producer;                       // 1초마다 보내는 워커
};

// "수신 창": 부모의 채널을 구독만 한다. 닫히면(OnDestroy) 구독은 자동 해제되고
// 그동안 받은 것은 채널에 쌓인다.
class ReceiveDialog : public nfc::Dialog {
public:
    ReceiveDialog(const std::shared_ptr<EchoDemo::Feed>& feed, int sw, int sh);
    int Run(nfc::Wnd* parent);

private:
    void OnMessage(const nfc::Bytes& m);

    std::shared_ptr<EchoDemo::Feed> m_feed;
    std::shared_ptr<nfc::Label> m_log;
    std::shared_ptr<nfc::Button> m_close;
    int m_count; // 이번에 열린 동안 받은 수
};

// IDD 리소스 폼: NFC_Demo.rc의 IDD_DEMO_FORM(리소스 편집기에서 편집)을 읽어 컨트롤을 만들고
// Bind로 멤버에 연결한 뒤 컨트롤에 직접 클릭 핸들러를 건다.
class DemoFormDialog : public nfc::Dialog {
public:
    DemoFormDialog();
    int Run(nfc::Wnd* parent); // 리소스를 못 읽으면 -1

private:
    std::shared_ptr<nfc::Label> m_status;
    std::shared_ptr<nfc::Button> m_hello;
    std::shared_ptr<nfc::Button> m_close;
    int m_clicks;
};

// 메인 데모 창: 인사 라벨, "계산"(RunAsync), "OK", "비활성", "수신 창" 버튼 + 에코 데모.
class DemoDialog : public nfc::Dialog {
public:
    DemoDialog();
    virtual ~DemoDialog();
    int Run(); // 모달 실행. 닫히면 에코 데모를 정리하고 결과 반환

private:
    void OnCalc();
    void OnReceive();
    void OnForm();

    int m_sw, m_sh; // 스킨 물리 크기 (배치 기준)
    std::shared_ptr<nfc::Label> m_hello;
    std::shared_ptr<nfc::Button> m_calc;
    std::shared_ptr<nfc::Button> m_disabled;
    std::shared_ptr<nfc::Button> m_ok;
    std::shared_ptr<nfc::Button> m_receive;
    std::shared_ptr<nfc::Button> m_form; // IDD 폼 열기
    EchoDemo m_echo; // 마지막 멤버: 가장 먼저 소멸 → 라벨보다 먼저 소켓을 닫는다
};

} // namespace demo
