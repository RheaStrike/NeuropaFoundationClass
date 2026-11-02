#include "DemoDialogs.h"
#include "resource.h"
#include "NinePatch.h"
#include "CString.h"
#include <chrono>
#include <string>

namespace demo {

namespace {
// UTF-8 <-> wstring (데모용)
std::string ToUtf8(const std::wstring& w) {
    return nfc::CString(w).ToUtf8();
}
std::wstring FromUtf8(const nfc::Bytes& b) {
    if (b.empty()) return std::wstring();
    int n = MultiByteToWideChar(CP_UTF8, 0, (const char*)&b[0], (int)b.size(), NULL, 0);
    std::wstring w((size_t)(n > 0 ? n : 0), L'\0');
    if (n > 0) MultiByteToWideChar(CP_UTF8, 0, (const char*)&b[0], (int)b.size(), &w[0], n);
    return w;
}
// 공통 라벨/버튼 생성
std::shared_ptr<nfc::Label> MakeTitle(int sw, const std::wstring& text) {
    std::shared_ptr<nfc::Label> l(new nfc::Label());
    l->SetBounds(20, 24, sw - 40, 28);
    l->SetAlign(1);
    l->SetTextColor(Gdiplus::Color(255, 32, 32, 32));
    l->SetText(text);
    return l;
}
std::shared_ptr<nfc::Button> MakeButton(int x, int y, const wchar_t* text) {
    std::shared_ptr<nfc::Button> b(new nfc::Button());
    b->SetBounds(x, y, 100, 32);
    b->SetText(text);
    return b;
}
} // namespace

// ===================== EchoDemo =====================
EchoDemo::EchoDemo() : m_echoCount(new int(0)), m_stop(false) {}

EchoDemo::~EchoDemo() { Stop(); }

void EchoDemo::Start(const std::shared_ptr<nfc::Label>& owner) {
    // 서버: 데모용 최소 에코 (I/O 스레드에서 받은 그대로 돌려보냄)
    m_listener = nfc::TcpListener::Create(0, true,
        [](const std::shared_ptr<nfc::Socket>& s) {
            s->SetIoReceiveHandler([](nfc::Socket& self, const nfc::Bytes& b) { self.Send(b); });
        });
    // 클라이언트: 기본 경로 = 소유자(라벨)가 자기 핸들러로 직접 받는다 (부모를 거치지 않음)
    m_socket = nfc::Socket::Create(nfc::Mode::Delegate);
    if (m_socket && m_listener) {
        std::shared_ptr<int> echoCount = m_echoCount;
        m_socket->SetHandlers<nfc::Label>(owner,
            [echoCount](nfc::Label& l, const nfc::Bytes& b) {
                ++*echoCount;
                l.SetText(L"에코 " + std::to_wstring(*echoCount) + L": " + FromUtf8(b));
                l.Invalidate();
            },
            [](nfc::Label& l, const nfc::NetEvent& e) {
                if (e.type == nfc::NET_ERROR) {
                    l.SetText(L"소켓 오류");
                    l.Invalidate();
                }
            });
        m_socket->Connect("127.0.0.1", m_listener->Port());
    }
    // 선택 경로: 수신 창이 닫혀 있는 동안 받은 것을 모아 두려고 채널에도 Push (최대 100개)
    m_feed = Feed::Create(100, Feed::DropOldest);
    if (m_socket) m_socket->SetChannels(m_feed, std::shared_ptr<nfc::Channel<nfc::NetEvent> >());
    // 송신: 워커 스레드가 1초마다 보낸다 (Send는 임의 스레드 가능)
    std::shared_ptr<nfc::Socket> sock = m_socket;
    m_stop = false;
    m_producer = std::thread([this, sock]() {
        int n = 0;
        while (!m_stop.load()) {
            std::this_thread::sleep_for(std::chrono::seconds(1));
            if (sock) sock->Send(ToUtf8(L"메시지 " + std::to_wstring(++n)));
        }
    });
}

void EchoDemo::Stop() {
    m_stop = true;
    if (m_producer.joinable()) m_producer.join();
    if (m_socket) { m_socket->Close(); m_socket.reset(); }
    if (m_listener) { m_listener->Close(); m_listener.reset(); }
}

// ===================== ReceiveDialog =====================
ReceiveDialog::ReceiveDialog(const std::shared_ptr<EchoDemo::Feed>& feed, int sw, int sh)
    : m_feed(feed), m_count(0) {
    m_log = MakeTitle(sw, L"대기 중 (쌓인 것 " +
        std::to_wstring(m_feed ? m_feed->Pending() : 0) + L"개)");
    AddControl(m_log);
    m_close = MakeButton((sw - 100) / 2, sh - 32 - 18, L"닫기");
    m_close->Click = [this]() { EndDialog(IDOK); };
    AddControl(m_close);
}

int ReceiveDialog::Run(nfc::Wnd* parent) {
    // 구독은 이 창 수명 안에서만 산다 (닫히면 자동 해제)
    Subscribe<nfc::Bytes>(m_feed, [this](const nfc::Bytes& m) { OnMessage(m); });
    return DoModal(parent, L"NFC_Demo - 수신", 0, 0);
}

void ReceiveDialog::OnMessage(const nfc::Bytes& m) {
    ++m_count;
    m_log->SetText(FromUtf8(m) + L"  (이번에 받은 수 " + std::to_wstring(m_count) + L")");
    m_log->Invalidate();
}

// ===================== DemoDialog =====================
// 창 크기는 스킨 물리 크기 그대로. 배치는 스킨 크기에 상대적으로.
DemoDialog::DemoDialog()
    : m_sw(nfc::NinePatch::Default()->ImageWidth()),
      m_sh(nfc::NinePatch::Default()->ImageHeight()) {
    const int by = m_sh - 32 - 18; // 아래 버튼 줄
    // 라벨: 스킨 없음 → 부모(다이얼로그)가 알려 준 자기 자리 스킨 위에 글자
    m_hello = MakeTitle(m_sw, L"Hello, NFC");
    AddControl(m_hello);
    // 계산 버튼: 워커에서 일하고 UI 스레드에서 결과를 받는다 (Control::RunAsync)
    // Click은 this만 잡는다 (버튼을 shared_ptr로 잡으면 순환 참조).
    m_calc = MakeButton(20, by, L"계산");
    m_calc->Click = [this]() { OnCalc(); };
    AddControl(m_calc);
    // 비활성 버튼: 비활성 상태 표시 예
    m_disabled = MakeButton(m_sw - 120, by, L"비활성");
    m_disabled->SetEnabled(false);
    AddControl(m_disabled);
    m_ok = MakeButton((m_sw - 100) / 2, by, L"OK");
    m_ok->Click = [this]() { EndDialog(IDOK); };
    AddControl(m_ok);
    m_receive = MakeButton((m_sw - 100) / 2, 60, L"수신 창");
    m_receive->Click = [this]() { OnReceive(); };
    AddControl(m_receive);
    // IDD 폼: 리소스 편집기에서 만든 IDD_DEMO_FORM을 띄운다
    m_form = MakeButton(20, 60, L"IDD 폼");
    m_form->Click = [this]() { OnForm(); };
    AddControl(m_form);
}

DemoDialog::~DemoDialog() {
    m_echo.Stop();
}

int DemoDialog::Run() {
    m_echo.Start(m_hello);
    int r = DoModal(NULL, L"NFC_Demo", 0, 0);
    m_echo.Stop();
    return r;
}

void DemoDialog::OnCalc() {
    // done은 m_calc가 살아 있을 때만 불리고, m_hello도 같은 다이얼로그 소속이다.
    m_calc->RunAsync([]() { return 40 + 2; },
        [this](std::shared_future<int> f) {
            m_hello->SetText(L"worker = " + std::to_wstring(f.get()));
            m_hello->Invalidate();
        });
}

void DemoDialog::OnReceive() {
    ReceiveDialog child(m_echo.GetFeed(), m_sw, m_sh);
    child.Run(this);
}

void DemoDialog::OnForm() {
    DemoFormDialog form;
    form.Run(this);
}

// ===================== DemoFormDialog =====================
DemoFormDialog::DemoFormDialog() : m_clicks(0) {}

int DemoFormDialog::Run(nfc::Wnd* parent) {
    // 1) 리소스 → 컨트롤 생성 (Custom Control 클래스 이름 NFC_Button/NFC_Label)
    if (!LoadFromResource(GetModuleHandle(NULL), IDD_DEMO_FORM)) return -1;
    // 2) ID로 멤버에 연결. 없거나 타입이 다르면 NULL (디버그 로그)
    Bind(IDC_FORM_STATUS, m_status);
    Bind(IDC_FORM_HELLO, m_hello);
    Bind(IDC_FORM_CLOSE, m_close);
    if (m_status) m_status->SetTextColor(Gdiplus::Color(255, 32, 32, 32));
    // 3) 이벤트는 컨트롤에 직접 건다 (WM_COMMAND 분기 없음)
    if (m_hello) {
        m_hello->Click = [this]() {
            ++m_clicks;
            if (m_status) {
                m_status->SetText(L"안녕하세요! (" + std::to_wstring(m_clicks) + L"번)");
                m_status->Invalidate();
            }
        };
    }
    if (m_close) m_close->Click = [this]() { EndDialog(IDOK); };
    return DoModal(parent, GetTemplateTitle().c_str(), GetTemplateWidth(), GetTemplateHeight());
}

} // namespace demo
