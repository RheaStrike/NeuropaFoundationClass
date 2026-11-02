#pragma once
#include "NetworkManager.h"
#include "Channel.h"
#include <memory>
#include <string>
#include <utility>
#include <vector>

// 공개 헤더. winhttp.h를 노출하지 않는다.

namespace nfc {

enum HttpError {
    HTTP_OK = 0,
    HTTP_ERR_URL,                // URL 형식 오류·지원 안 하는 스킴·URL 안 계정정보
    HTTP_ERR_BAD_REQUEST,        // 헤더에 CR/LF 등 잘못된 요청
    HTTP_ERR_CONNECT,            // 이름 해석·연결 실패
    HTTP_ERR_TIMEOUT,            // 연결/송신/수신/전체 타임아웃
    HTTP_ERR_TLS,                // 인증서·TLS 오류 (검증은 항상 켜짐)
    HTTP_ERR_TOO_LARGE,          // 응답 본문 한도 초과
    HTTP_ERR_HEADER_TOO_LARGE,   // 응답 헤더 한도 초과
    HTTP_ERR_REDIRECT_LIMIT,     // 리다이렉트 횟수 초과
    HTTP_ERR_INSECURE_REDIRECT,  // https → http 다운그레이드 리다이렉트
    HTTP_ERR_CANCELLED,          // Cancel()
    HTTP_ERR_SHUTDOWN,           // 앱/매니저 종료
    HTTP_ERR_IO,                 // 기타 전송 오류
    HTTP_ERR_INTERNAL            // 콜백 예외 등 내부 오류
};

struct HttpHeader {
    std::wstring name;
    std::wstring value;
};

struct NFC_API HttpUrl {
    HttpUrl() : secure(false), port(0) {}
    bool secure;
    std::wstring host;  // IPv6는 대괄호 없이
    unsigned short port;
    std::wstring path;  // 경로+쿼리 (조각 제거), 최소 "/"
    // http/https만. 계정정보(user@host) 거부, 포트 1~65535
    static bool Parse(const std::wstring& url, HttpUrl& out);
    std::wstring ToString() const;
};

struct HttpRequest {
    HttpRequest() : method(L"GET"), mode(Mode::Delegate), connectTimeoutMs(10000),
        sendTimeoutMs(30000), receiveTimeoutMs(30000), totalTimeoutMs(60000),
        maxBodyBytes(16u * 1024u * 1024u), maxHeaderBytes(64u * 1024u), maxRedirects(5) {}
    std::wstring method;               // GET/POST/PUT/DELETE/... (문자열)
    std::wstring url;
    std::vector<HttpHeader> headers;
    Bytes body;
    Mode mode;                         // Delegate(공용 세션+큐) / Own(요청 전용 세션)
    int connectTimeoutMs;
    int sendTimeoutMs;
    int receiveTimeoutMs;
    int totalTimeoutMs;
    size_t maxBodyBytes;
    size_t maxHeaderBytes;
    int maxRedirects;
};

struct HttpResult {
    HttpResult() : status(0), error(HTTP_OK), systemError(0), redirects(0) {}
    int status;
    std::vector<HttpHeader> headers;
    Bytes body;
    int error;            // HttpError
    unsigned systemError; // WinHTTP/Win32 오류 코드
    std::string message;  // ASCII. 쿠키·Authorization 값은 절대 넣지 않는다
    std::wstring finalUrl;
    int redirects;
    bool Ok() const { return error == HTTP_OK; }
    std::string BodyText() const { return std::string(body.begin(), body.end()); }
};

// 실제로 적용되는 보안 옵션 (조회 전용. 끄는 API는 없다)
struct HttpSecurityOptions {
    bool certificateValidation; // 항상 true
    bool tls12Minimum;          // TLS 1.2 이상만
    unsigned secureProtocols;   // WINHTTP_FLAG_SECURE_PROTOCOL_* 값
    bool autoRedirect;          // false: 리다이렉트는 직접 처리(횟수·다운그레이드 검사)
    bool cookies;               // false: 쿠키 자동 저장/전송 끔
};

// 요청 핸들. Cancel은 어느 스레드에서 몇 번 불러도 안전.
class NFC_API HttpCall {
public:
    virtual ~HttpCall() {}
    virtual void Cancel() = 0;
    virtual bool IsDone() const = 0;
};

// 안전한 HTTP 클라이언트 (WinHTTP 비동기, HTTPS는 Windows 기본 TLS/SChannel).
// 결과는 "요청한 객체 자신"에게 직접 간다: Send(req, owner, handler) → UI 스레드에서
// handler(*owner, result). 소유자는 weak로 잡고, 사라졌으면 결과를 버리고 요청을 취소한다.
// 여러 곳이 같은 결과를 봐야 할 때만 alsoPush 채널을 추가로 준다.
class NFC_API HttpClient {
public:
    static bool Start();      // App::Init에서 자동
    static void Shutdown();   // App::Shutdown에서 자동: 진행 중 요청 전부 취소 후 핸들 정리
    static bool IsRunning();
    static void SetMaxConcurrent(int n); // Delegate 동시 요청 상한 (기본 4), 넘치면 큐
    static int ActiveCount();  // 진행 중 (Own 포함)
    static int QueuedCount();

    // 임의 스레드 호출 가능. 멈춰 있으면 NULL (핸들러 안 불림).
    template <typename O>
    static std::shared_ptr<HttpCall> Send(const HttpRequest& req, const std::shared_ptr<O>& owner,
            std::function<void(O&, const HttpResult&)> handler,
            std::shared_ptr<Channel<HttpResult> > alsoPush = std::shared_ptr<Channel<HttpResult> >()) {
        std::weak_ptr<O> w = owner;
        std::function<bool()> alive = [w]() { return !w.expired(); };
        std::function<bool(const HttpResult&)> deliver = [w, handler](const HttpResult& r) {
            std::shared_ptr<O> o = w.lock();
            if (!o) return false;
            if (handler) handler(*o, r);
            return true;
        };
        return SendImpl(req, alive, deliver, alsoPush);
    }

    static HttpSecurityOptions SecurityOptions();
    // 로그용: Authorization/Proxy-Authorization/Cookie/Set-Cookie 값은 "***"
    static std::wstring RedactHeaderForLog(const std::wstring& name, const std::wstring& value);
    // 리다이렉트 허용 여부 (https → http 금지)
    static bool IsRedirectAllowed(const HttpUrl& from, const HttpUrl& to);
    // Location(절대/스킴 상대/경로 상대)을 base 기준으로 해석
    static bool ResolveRedirect(const HttpUrl& base, const std::wstring& location, HttpUrl& out);

private:
    static std::shared_ptr<HttpCall> SendImpl(const HttpRequest& req,
        std::function<bool()> alive, std::function<bool(const HttpResult&)> deliver,
        std::shared_ptr<Channel<HttpResult> > alsoPush);
};

} // namespace nfc
