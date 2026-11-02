#include "framework.h"
#include <winhttp.h>
#pragma comment(lib, "winhttp.lib")
#include "HttpClient.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>

namespace nfc {

namespace {

#ifndef WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3
#define WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3 0x00002000
#endif
const DWORD kTls12 = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_2;
const DWORD kTls13 = WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_3;

// WinHTTP 핸들 RAII (세션용)
struct InternetHandle {
    InternetHandle() : h(NULL) {}
    ~InternetHandle() { Reset(); }
    void Reset(HINTERNET n = NULL) {
        if (h) WinHttpCloseHandle(h);
        h = n;
    }
    HINTERNET h;
};

std::string Ascii(const std::wstring& w) {
    std::string s;
    for (size_t i = 0; i < w.size(); ++i) s += (w[i] < 128 && w[i] >= 32) ? (char)w[i] : '?';
    return s;
}

std::wstring Lower(std::wstring s) {
    for (size_t i = 0; i < s.size(); ++i) if (s[i] >= L'A' && s[i] <= L'Z') s[i] = s[i] - L'A' + L'a';
    return s;
}

bool HasCrLf(const std::wstring& s) {
    return s.find_first_of(L"\r\n") != std::wstring::npos || s.find(L'\0') != std::wstring::npos;
}

bool IsLoopbackHost(const std::wstring& h) {
    std::wstring l = Lower(h);
    return l == L"localhost" || l == L"127.0.0.1" || l == L"::1";
}

int MapError(DWORD e) {
    switch (e) {
    case ERROR_WINHTTP_TIMEOUT: return HTTP_ERR_TIMEOUT;
    case ERROR_WINHTTP_OPERATION_CANCELLED: return HTTP_ERR_CANCELLED;
    case ERROR_WINHTTP_NAME_NOT_RESOLVED:
    case ERROR_WINHTTP_CANNOT_CONNECT:
    case ERROR_WINHTTP_CONNECTION_ERROR: return HTTP_ERR_CONNECT;
    case ERROR_WINHTTP_SECURE_FAILURE:
    case ERROR_WINHTTP_SECURE_CERT_DATE_INVALID:
    case ERROR_WINHTTP_SECURE_CERT_CN_INVALID:
    case ERROR_WINHTTP_SECURE_INVALID_CA:
    case ERROR_WINHTTP_SECURE_CERT_REV_FAILED:
    case ERROR_WINHTTP_SECURE_CHANNEL_ERROR:
    case ERROR_WINHTTP_SECURE_INVALID_CERT:
    case ERROR_WINHTTP_SECURE_CERT_REVOKED:
    case ERROR_WINHTTP_SECURE_CERT_WRONG_USAGE: return HTTP_ERR_TLS;
    case ERROR_WINHTTP_HEADER_SIZE_OVERFLOW: return HTTP_ERR_HEADER_TOO_LARGE;
    default: return HTTP_ERR_IO;
    }
}

class CallImpl;

struct Global {
    Global() : running(false), maxConcurrent(4), active(0), liveHandles(0), protocols(0) {}
    std::mutex m;
    InternetHandle session;            // Delegate 공용 세션
    bool running;
    int maxConcurrent;
    int active;                        // Delegate 진행 중
    std::deque<std::shared_ptr<CallImpl> > queue;
    std::vector<std::weak_ptr<CallImpl> > all;
    int liveHandles;                   // 닫히기를 기다리는 요청 핸들 수 (Own 세션 포함)
    std::condition_variable idle;
    DWORD protocols;                   // 실제 적용된 TLS 플래그
};

Global& G() {
    static Global g;
    return g;
}

void CALLBACK StatusCallback(HINTERNET h, DWORD_PTR ctx, DWORD status, LPVOID info, DWORD len);

// 세션 열기 + 보안 기본값 (TLS1.2 이상, 콜백). 인증서 검증은 기본(켜짐)을 건드리지 않는다.
HINTERNET OpenSession(DWORD* appliedProtocols) {
    HINTERNET s = WinHttpOpen(L"NFC/1.0", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY,
        WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    if (!s) {
        s = WinHttpOpen(L"NFC/1.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, WINHTTP_FLAG_ASYNC);
    }
    if (!s) return NULL;
    DWORD prot = kTls12 | kTls13;
    if (!WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &prot, sizeof(prot))) {
        prot = kTls12; // TLS1.3 미지원 OS
        if (!WinHttpSetOption(s, WINHTTP_OPTION_SECURE_PROTOCOLS, &prot, sizeof(prot))) {
            WinHttpCloseHandle(s);
            return NULL; // 최소 TLS1.2를 보장 못 하면 세션을 쓰지 않는다
        }
    }
    if (appliedProtocols) *appliedProtocols = prot;
    if (WinHttpSetStatusCallback(s, &StatusCallback,
            WINHTTP_CALLBACK_FLAG_ALL_COMPLETIONS | WINHTTP_CALLBACK_FLAG_HANDLES, 0)
            == WINHTTP_INVALID_STATUS_CALLBACK) {
        WinHttpCloseHandle(s);
        return NULL;
    }
    return s;
}

// 요청 하나. 수명: 사용자 핸들 + 열린 요청 핸들(HANDLE_CLOSING까지) + 큐가 붙잡는다.
class CallImpl : public HttpCall, public std::enable_shared_from_this<CallImpl> {
public:
    CallImpl(const HttpRequest& r, std::function<bool()> alive,
            std::function<bool(const HttpResult&)> deliver,
            std::shared_ptr<Channel<HttpResult> > ch)
        : m_req(r), m_alive(alive), m_deliver(deliver), m_ch(ch), m_session(NULL),
          m_connect(NULL), m_request(NULL), m_finished(false), m_started(false),
          m_queued(false), m_status(0) {}

    virtual void Cancel() { Abort(HTTP_ERR_CANCELLED, "cancelled"); }
    virtual bool IsDone() const { return m_finished.load(); }

    bool Prepare(HttpResult& err) {
        if (!HttpUrl::Parse(m_req.url, m_url)) {
            err.error = HTTP_ERR_URL;
            err.message = "invalid url";
            return false;
        }
        if (HasCrLf(m_req.method) || m_req.method.empty()) {
            err.error = HTTP_ERR_BAD_REQUEST;
            err.message = "invalid method";
            return false;
        }
        for (size_t i = 0; i < m_req.headers.size(); ++i) {
            const HttpHeader& h = m_req.headers[i];
            if (h.name.empty() || HasCrLf(h.name) || HasCrLf(h.value) ||
                    h.name.find(L':') != std::wstring::npos) {
                err.error = HTTP_ERR_BAD_REQUEST;
                err.message = "invalid header: " + Ascii(h.name); // 값은 로그에 넣지 않는다
                return false;
            }
        }
        return true;
    }

    // 큐에서 슬롯을 받았을 때(또는 Own이면 바로) 시작
    void Begin() {
        std::shared_ptr<CallImpl> keep = shared_from_this();
        if (m_finished.load()) return;
        if (m_req.totalTimeoutMs > 0) {
            std::weak_ptr<CallImpl> w = keep;
            NetworkManager::RunAfter(m_req.totalTimeoutMs, [w]() {
                std::shared_ptr<CallImpl> c = w.lock();
                if (c) c->Abort(HTTP_ERR_TIMEOUT, "total timeout");
            });
        }
        std::lock_guard<std::recursive_mutex> l(m_mutex);
        OpenAndSend();
    }

    void Abort(int error, const std::string& msg) {
        HttpResult r;
        r.error = error;
        r.message = msg;
        Finish(r, true);
    }

    // 상태 콜백 (WinHTTP 스레드 풀). 예외는 StatusCallback이 막는다.
    void OnStatus(HINTERNET h, DWORD status, LPVOID info, DWORD len) {
        std::shared_ptr<CallImpl> keep = shared_from_this();
        std::lock_guard<std::recursive_mutex> l(m_mutex);
        if (m_finished.load() || h != m_request) return; // 끝났거나 리다이렉트 전 옛 핸들
        if (!m_alive()) { // 소유자 소멸 → 조용히 취소
            Finish(Fail(HTTP_ERR_CANCELLED, "owner gone"), false);
            return;
        }
        ++m_progress; // 수신 감시: 진행이 있으면 타이머를 늦춘다
        switch (status) {
        case WINHTTP_CALLBACK_STATUS_SENDREQUEST_COMPLETE:
            if (!WinHttpReceiveResponse(m_request, NULL)) FailLast("receive response");
            break;
        case WINHTTP_CALLBACK_STATUS_HEADERS_AVAILABLE:
            OnHeaders();
            break;
        case WINHTTP_CALLBACK_STATUS_DATA_AVAILABLE: {
            DWORD n = info ? *reinterpret_cast<DWORD*>(info) : 0;
            if (n == 0) {
                Complete();
            } else if (m_body.size() + n > m_req.maxBodyBytes) {
                Finish(Fail(HTTP_ERR_TOO_LARGE, "response body too large"), true);
            } else {
                m_buf.resize(n);
                if (!WinHttpReadData(m_request, &m_buf[0], n, NULL)) FailLast("read");
            }
            break;
        }
        case WINHTTP_CALLBACK_STATUS_READ_COMPLETE:
            if (len == 0) {
                Complete();
            } else {
                m_body.insert(m_body.end(), m_buf.begin(), m_buf.begin() + len);
                if (m_body.size() > m_req.maxBodyBytes) {
                    Finish(Fail(HTTP_ERR_TOO_LARGE, "response body too large"), true);
                } else if (!WinHttpQueryDataAvailable(m_request, NULL)) {
                    FailLast("query data");
                }
            }
            break;
        case WINHTTP_CALLBACK_STATUS_REQUEST_ERROR: {
            WINHTTP_ASYNC_RESULT* ar = reinterpret_cast<WINHTTP_ASYNC_RESULT*>(info);
            DWORD e = ar ? ar->dwError : 0;
            HttpResult r = Fail(MapError(e), "request error");
            r.systemError = e;
            Finish(r, true);
            break;
        }
        default:
            break;
        }
    }

    // HANDLE_CLOSING: 이 핸들에 대한 마지막 콜백. 붙잡은 참조를 돌려준다.
    std::shared_ptr<CallImpl> TakeHandleRef(HINTERNET h) {
        std::lock_guard<std::recursive_mutex> l(m_mutex);
        std::shared_ptr<CallImpl> out;
        for (size_t i = 0; i < m_handleRefs.size(); ++i) {
            if (m_handleRefs[i].first == h) {
                out = m_handleRefs[i].second;
                m_handleRefs.erase(m_handleRefs.begin() + i);
                break;
            }
        }
        if (out) {
            std::lock_guard<std::mutex> gl(G().m);
            --G().liveHandles;
            G().idle.notify_all();
        }
        return out;
    }

    bool IsOwn() const { return m_req.mode == Mode::Own; }
    void SetQueued(bool q) { m_queued = q; }
    bool Started() const { return m_started; }
    void MarkStarted() { m_started = true; }

private:
    HttpResult Fail(int error, const std::string& msg) {
        HttpResult r;
        r.error = error;
        r.message = msg + " (" + Ascii(m_url.host) + ")";
        return r;
    }

    void FailLast(const char* what) {
        DWORD e = GetLastError();
        HttpResult r = Fail(MapError(e), what);
        r.systemError = e;
        Finish(r, true);
    }

    // 잠금 상태에서. 현재 m_url로 핸들을 열고 보낸다.
    void OpenAndSend() {
        HINTERNET session = G().session.h;
        if (IsOwn()) {
            if (!m_ownSession.h) {
                DWORD prot = 0;
                m_ownSession.Reset(OpenSession(&prot));
            }
            session = m_ownSession.h;
        }
        if (!session) {
            Finish(Fail(HTTP_ERR_SHUTDOWN, "no session"), true);
            return;
        }
        m_connect = WinHttpConnect(session, m_url.host.c_str(), m_url.port, 0);
        if (!m_connect) {
            FailLast("connect handle");
            return;
        }
        m_request = WinHttpOpenRequest(m_connect, m_req.method.c_str(), m_url.path.c_str(), NULL,
            WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES,
            m_url.secure ? WINHTTP_FLAG_SECURE : 0);
        if (!m_request) {
            FailLast("open request");
            return;
        }
        // 이 핸들이 닫힐 때까지 객체를 붙잡는다
        m_handleRefs.push_back(std::make_pair(m_request, shared_from_this()));
        {
            std::lock_guard<std::mutex> gl(G().m);
            ++G().liveHandles;
        }
        DWORD_PTR ctx = reinterpret_cast<DWORD_PTR>(this);
        WinHttpSetOption(m_request, WINHTTP_OPTION_CONTEXT_VALUE, &ctx, sizeof(ctx));
        DWORD v = WINHTTP_OPTION_REDIRECT_POLICY_NEVER; // 리다이렉트는 직접 검사
        WinHttpSetOption(m_request, WINHTTP_OPTION_REDIRECT_POLICY, &v, sizeof(v));
        v = WINHTTP_DISABLE_COOKIES;
        WinHttpSetOption(m_request, WINHTTP_OPTION_DISABLE_FEATURE, &v, sizeof(v));
#ifdef WINHTTP_OPTION_MAX_RESPONSE_HEADER_SIZE
        v = (DWORD)m_req.maxHeaderBytes;
        WinHttpSetOption(m_request, WINHTTP_OPTION_MAX_RESPONSE_HEADER_SIZE, &v, sizeof(v));
#endif
        if (IsLoopbackHost(m_url.host)) { // loopback은 프록시를 거치지 않는다
            WINHTTP_PROXY_INFO pi;
            ZeroMemory(&pi, sizeof(pi));
            pi.dwAccessType = WINHTTP_ACCESS_TYPE_NO_PROXY;
            WinHttpSetOption(m_request, WINHTTP_OPTION_PROXY, &pi, sizeof(pi));
        }
        WinHttpSetTimeouts(m_request, m_req.connectTimeoutMs, m_req.connectTimeoutMs,
            m_req.sendTimeoutMs, m_req.receiveTimeoutMs);
        // 응답 헤더 대기 시간은 별도 옵션(기본 90초)이라 수신 타임아웃과 맞춘다
        v = (DWORD)m_req.receiveTimeoutMs;
        WinHttpSetOption(m_request, WINHTTP_OPTION_RECEIVE_RESPONSE_TIMEOUT, &v, sizeof(v));
        std::wstring hdrs;
        for (size_t i = 0; i < m_req.headers.size(); ++i)
            hdrs += m_req.headers[i].name + L": " + m_req.headers[i].value + L"\r\n";
        LogStart();
        m_status = 0;
        m_body.clear();
        m_headers.clear();
        LPVOID body = m_req.body.empty() ? WINHTTP_NO_REQUEST_DATA : (LPVOID)&m_req.body[0];
        DWORD blen = (DWORD)m_req.body.size();
        if (!WinHttpSendRequest(m_request, hdrs.empty() ? WINHTTP_NO_ADDITIONAL_HEADERS : hdrs.c_str(),
                hdrs.empty() ? 0 : (DWORD)-1L, body, blen, blen, ctx)) {
            FailLast("send request");
            return;
        }
        // SENDREQUEST_COMPLETE는 응답 직전까지 늦게 올 수 있어 보낸 직후부터 감시한다
        ArmReceiveWatch();
    }

    // WinHTTP 수신 타임아웃은 비동기 응답 대기에서 보장되지 않아 직접 감시한다.
    // 보낸 뒤 receiveTimeoutMs(연결·송신 시간 포함) 동안 아무 진행(헤더/데이터)이 없으면 TIMEOUT으로 끝낸다.
    void ArmReceiveWatch() {
        if (m_watchArmed || m_req.receiveTimeoutMs <= 0) return;
        m_watchArmed = true;
        ScheduleWatch(m_progress.load());
    }

    void ScheduleWatch(long seen) {
        std::weak_ptr<CallImpl> w = shared_from_this();
        NetworkManager::RunAfter(m_req.receiveTimeoutMs, [w, seen]() {
            std::shared_ptr<CallImpl> c = w.lock();
            if (!c || c->IsDone()) return;
            long now = c->m_progress.load();
            if (now == seen) c->Abort(HTTP_ERR_TIMEOUT, "receive timeout");
            else c->ScheduleWatch(now);
        });
    }

    void LogStart() {
        // 쿼리(토큰이 있을 수 있음)와 민감 헤더 값은 남기지 않는다
        std::wstring path = m_url.path.substr(0, m_url.path.find(L'?'));
        std::wstring line = L"NFC http: " + m_req.method + L" " + m_url.host + path;
        for (size_t i = 0; i < m_req.headers.size(); ++i)
            line += L" | " + m_req.headers[i].name + L"=" +
                HttpClient::RedactHeaderForLog(m_req.headers[i].name, m_req.headers[i].value);
        line += L"\n";
        OutputDebugStringW(line.c_str());
    }

    void OnHeaders() {
        DWORD code = 0, sz = sizeof(code);
        if (!WinHttpQueryHeaders(m_request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX, &code, &sz, WINHTTP_NO_HEADER_INDEX)) {
            FailLast("status");
            return;
        }
        m_status = (int)code;
        DWORD bytes = 0;
        WinHttpQueryHeaders(m_request, WINHTTP_QUERY_RAW_HEADERS_CRLF, WINHTTP_HEADER_NAME_BY_INDEX,
            NULL, &bytes, WINHTTP_NO_HEADER_INDEX);
        if (bytes > m_req.maxHeaderBytes * sizeof(wchar_t)) {
            Finish(Fail(HTTP_ERR_HEADER_TOO_LARGE, "response headers too large"), true);
            return;
        }
        if (bytes > 0) {
            std::vector<wchar_t> raw(bytes / sizeof(wchar_t) + 1, 0);
            if (WinHttpQueryHeaders(m_request, WINHTTP_QUERY_RAW_HEADERS_CRLF,
                    WINHTTP_HEADER_NAME_BY_INDEX, &raw[0], &bytes, WINHTTP_NO_HEADER_INDEX)) {
                ParseHeaders(std::wstring(&raw[0]));
            }
        }
        if (m_status >= 300 && m_status < 400 && m_status != 304) {
            std::wstring loc;
            for (size_t i = 0; i < m_headers.size(); ++i)
                if (Lower(m_headers[i].name) == L"location") loc = m_headers[i].value;
            if (!loc.empty()) {
                Redirect(loc);
                return;
            }
        }
        if (!WinHttpQueryDataAvailable(m_request, NULL)) FailLast("query data");
    }

    void ParseHeaders(const std::wstring& raw) {
        size_t p = raw.find(L"\r\n"); // 첫 줄(상태 줄) 건너뜀
        while (p != std::wstring::npos) {
            size_t s = p + 2;
            p = raw.find(L"\r\n", s);
            std::wstring line = raw.substr(s, p == std::wstring::npos ? std::wstring::npos : p - s);
            size_t c = line.find(L':');
            if (c == std::wstring::npos || c == 0) continue;
            HttpHeader h;
            h.name = line.substr(0, c);
            size_t v = line.find_first_not_of(L" \t", c + 1);
            h.value = v == std::wstring::npos ? L"" : line.substr(v);
            m_headers.push_back(h);
        }
    }

    void Redirect(const std::wstring& loc) {
        HttpUrl next;
        if (++m_redirects > m_req.maxRedirects) {
            Finish(Fail(HTTP_ERR_REDIRECT_LIMIT, "too many redirects"), true);
            return;
        }
        if (!HttpClient::ResolveRedirect(m_url, loc, next)) {
            Finish(Fail(HTTP_ERR_URL, "bad redirect location"), true);
            return;
        }
        if (!HttpClient::IsRedirectAllowed(m_url, next)) {
            Finish(Fail(HTTP_ERR_INSECURE_REDIRECT, "https to http redirect blocked"), true);
            return;
        }
        if (m_status == 301 || m_status == 302 || m_status == 303) {
            if (m_req.method != L"HEAD") m_req.method = L"GET";
            m_req.body.clear();
        }
        // 다른 호스트로 가면 Authorization/Cookie를 넘기지 않는다
        if (Lower(next.host) != Lower(m_url.host) || next.port != m_url.port) {
            std::vector<HttpHeader> kept;
            for (size_t i = 0; i < m_req.headers.size(); ++i) {
                std::wstring n = Lower(m_req.headers[i].name);
                if (n != L"authorization" && n != L"cookie" && n != L"proxy-authorization")
                    kept.push_back(m_req.headers[i]);
            }
            m_req.headers.swap(kept);
        }
        CloseHandles(); // 옛 핸들의 늦은 콜백은 h != m_request로 무시된다
        m_url = next;
        OpenAndSend();
    }

    void Complete() {
        HttpResult r;
        r.status = m_status;
        r.headers = m_headers;
        r.body.swap(m_body);
        r.finalUrl = m_url.ToString();
        r.redirects = m_redirects;
        Finish(r, true);
    }

    void CloseHandles() {
        HINTERNET req = m_request, con = m_connect;
        m_request = NULL;
        m_connect = NULL;
        if (req) WinHttpCloseHandle(req); // 진행 중 작업 취소. HANDLE_CLOSING이 뒤따른다
        if (con) WinHttpCloseHandle(con);
    }

    void Finish(HttpResult r, bool deliver) {
        std::shared_ptr<CallImpl> keep = shared_from_this();
        {
            std::lock_guard<std::recursive_mutex> l(m_mutex);
            if (m_finished.exchange(true)) return;
            CloseHandles();
        }
        ReleaseSlot();
        if (r.finalUrl.empty()) r.finalUrl = m_url.ToString();
        r.redirects = m_redirects;
        if (!deliver || !m_alive()) return; // 소유자 소멸 → 조용히 버린다
        if (m_ch) m_ch->Push(r);
        std::function<bool(const HttpResult&)> d = m_deliver;
        App::PostToUI([d, r]() { d(r); });
    }

    void ReleaseSlot();

    HttpRequest m_req;
    HttpUrl m_url;
    std::function<bool()> m_alive;
    std::function<bool(const HttpResult&)> m_deliver;
    std::shared_ptr<Channel<HttpResult> > m_ch;
    std::recursive_mutex m_mutex; // CloseHandle 안에서 콜백이 다시 들어올 수 있어 재귀
    HINTERNET m_session;
    InternetHandle m_ownSession;  // Mode::Own 전용 세션
    HINTERNET m_connect;
    HINTERNET m_request;
    std::vector<std::pair<HINTERNET, std::shared_ptr<CallImpl> > > m_handleRefs;
    std::atomic<bool> m_finished;
    bool m_started;               // G().m 아래에서
    bool m_queued;                // G().m 아래에서
    int m_status;
    int m_redirects = 0;
    std::atomic<long> m_progress{0};
    bool m_watchArmed = false;
    std::vector<HttpHeader> m_headers;
    Bytes m_body;
    std::vector<char> m_buf;
};

void CallImpl::ReleaseSlot() {
    std::shared_ptr<CallImpl> next;
    {
        std::lock_guard<std::mutex> l(G().m);
        if (m_queued) { // 시작 전 취소: 큐에서만 뺀다
            for (size_t i = 0; i < G().queue.size(); ++i) {
                if (G().queue[i].get() == this) {
                    G().queue.erase(G().queue.begin() + i);
                    break;
                }
            }
            m_queued = false;
        } else if (m_started && !IsOwn()) {
            --G().active;
            while (G().running && !G().queue.empty() && G().active < G().maxConcurrent) {
                next = G().queue.front();
                G().queue.pop_front();
                next->SetQueued(false);
                if (next->IsDone()) { next.reset(); continue; }
                next->MarkStarted();
                ++G().active;
                break;
            }
        }
        m_started = false;
        G().idle.notify_all();
    }
    if (next) next->Begin();
}

void CALLBACK StatusCallback(HINTERNET h, DWORD_PTR ctx, DWORD status, LPVOID info, DWORD len) {
    CallImpl* c = reinterpret_cast<CallImpl*>(ctx);
    if (!c) return; // 세션·연결 핸들
    try {
        if (status == WINHTTP_CALLBACK_STATUS_HANDLE_CLOSING) {
            std::shared_ptr<CallImpl> last = c->TakeHandleRef(h);
            return; // last가 마지막 참조면 여기서 파괴된다 (멤버 접근 없음)
        }
        c->OnStatus(h, status, info, len);
    } catch (...) {
        // WinHTTP 스레드로 예외가 새지 않게 막는다
        try { c->Abort(HTTP_ERR_INTERNAL, "callback exception"); } catch (...) {}
    }
}

} // namespace

// ---------------------------------------------------------------------------

bool HttpUrl::Parse(const std::wstring& url, HttpUrl& out) {
    HttpUrl u;
    std::wstring s = url;
    size_t se = s.find(L"://");
    if (se == std::wstring::npos) return false;
    std::wstring scheme = Lower(s.substr(0, se));
    if (scheme == L"http") u.secure = false;
    else if (scheme == L"https") u.secure = true;
    else return false;
    std::wstring rest = s.substr(se + 3);
    size_t frag = rest.find(L'#');
    if (frag != std::wstring::npos) rest.resize(frag);
    size_t ps = rest.find_first_of(L"/?");
    std::wstring auth = rest.substr(0, ps);
    u.path = ps == std::wstring::npos ? L"/" : rest.substr(ps);
    if (u.path[0] == L'?') u.path = L"/" + u.path;
    if (auth.empty() || auth.find(L'@') != std::wstring::npos) return false; // 계정정보 거부
    if (HasCrLf(url) || url.find_first_of(L" \t") != std::wstring::npos) return false;
    std::wstring portStr;
    if (auth[0] == L'[') {
        size_t rb = auth.find(L']');
        if (rb == std::wstring::npos) return false;
        u.host = auth.substr(1, rb - 1);
        if (rb + 1 < auth.size()) {
            if (auth[rb + 1] != L':') return false;
            portStr = auth.substr(rb + 2);
        }
    } else {
        size_t c = auth.find(L':');
        u.host = auth.substr(0, c);
        if (c != std::wstring::npos) portStr = auth.substr(c + 1);
    }
    if (u.host.empty()) return false;
    if (!portStr.empty()) {
        if (portStr.size() > 5 || portStr.find_first_not_of(L"0123456789") != std::wstring::npos)
            return false;
        long p = wcstol(portStr.c_str(), NULL, 10);
        if (p < 1 || p > 65535) return false;
        u.port = (unsigned short)p;
    } else if (auth.size() && auth[auth.size() - 1] == L':') {
        return false;
    } else {
        u.port = u.secure ? 443 : 80;
    }
    out = u;
    return true;
}

std::wstring HttpUrl::ToString() const {
    std::wstring h = host.find(L':') != std::wstring::npos ? L"[" + host + L"]" : host;
    std::wstring s = (secure ? L"https://" : L"http://") + h;
    if (port != (secure ? 443 : 80)) s += L":" + std::to_wstring(port);
    return s + path;
}

bool HttpClient::IsRedirectAllowed(const HttpUrl& from, const HttpUrl& to) {
    return !(from.secure && !to.secure);
}

bool HttpClient::ResolveRedirect(const HttpUrl& base, const std::wstring& loc, HttpUrl& out) {
    if (loc.empty() || HasCrLf(loc)) return false;
    if (loc.find(L"://") != std::wstring::npos) return HttpUrl::Parse(loc, out);
    if (loc.size() > 1 && loc[0] == L'/' && loc[1] == L'/')
        return HttpUrl::Parse((base.secure ? L"https:" : L"http:") + loc, out);
    HttpUrl u = base;
    if (loc[0] == L'/') {
        u.path = loc;
    } else {
        std::wstring dir = base.path.substr(0, base.path.find(L'?'));
        dir = dir.substr(0, dir.rfind(L'/') + 1);
        u.path = dir + loc;
    }
    size_t frag = u.path.find(L'#');
    if (frag != std::wstring::npos) u.path.resize(frag);
    out = u;
    return true;
}

std::wstring HttpClient::RedactHeaderForLog(const std::wstring& name, const std::wstring& value) {
    std::wstring n = Lower(name);
    if (n == L"authorization" || n == L"proxy-authorization" || n == L"cookie" ||
            n == L"set-cookie" || n == L"x-api-key")
        return L"***";
    return value;
}

HttpSecurityOptions HttpClient::SecurityOptions() {
    HttpSecurityOptions o;
    o.certificateValidation = true; // WinHTTP 기본 검증. 끄는 경로(SECURITY_FLAGS)는 코드에 없다
    {
        std::lock_guard<std::mutex> l(G().m);
        o.secureProtocols = G().protocols ? G().protocols : (kTls12 | kTls13);
    }
    o.tls12Minimum = (o.secureProtocols & (WINHTTP_FLAG_SECURE_PROTOCOL_SSL2 |
        WINHTTP_FLAG_SECURE_PROTOCOL_SSL3 | WINHTTP_FLAG_SECURE_PROTOCOL_TLS1 |
        WINHTTP_FLAG_SECURE_PROTOCOL_TLS1_1)) == 0 && (o.secureProtocols & kTls12) != 0;
    o.autoRedirect = false;
    o.cookies = false;
    return o;
}

bool HttpClient::Start() {
    Global& g = G();
    std::lock_guard<std::mutex> l(g.m);
    if (g.running) return true;
    DWORD prot = 0;
    g.session.Reset(OpenSession(&prot));
    if (!g.session.h) return false;
    g.protocols = prot;
    g.running = true;
    return true;
}

void HttpClient::Shutdown() {
    Global& g = G();
    std::vector<std::weak_ptr<CallImpl> > all;
    {
        std::lock_guard<std::mutex> l(g.m);
        if (!g.running) return;
        g.running = false; // 이후 Send 거부
        all.swap(g.all);
    }
    for (size_t i = 0; i < all.size(); ++i) {
        std::shared_ptr<CallImpl> c = all[i].lock();
        if (c) c->Abort(HTTP_ERR_SHUTDOWN, "shutdown");
    }
    {
        // 모든 요청 핸들의 HANDLE_CLOSING을 기다린 뒤 세션을 닫는다 (최대 5초)
        std::unique_lock<std::mutex> l(g.m);
        g.idle.wait_for(l, std::chrono::seconds(5),
            [&g]() { return g.liveHandles <= 0 && g.queue.empty(); });
        g.queue.clear();
        g.active = 0;
    }
    HINTERNET s = g.session.h;
    g.session.h = NULL;
    if (s) {
        WinHttpSetStatusCallback(s, NULL, 0, 0);
        WinHttpCloseHandle(s);
    }
}

bool HttpClient::IsRunning() {
    std::lock_guard<std::mutex> l(G().m);
    return G().running;
}

void HttpClient::SetMaxConcurrent(int n) {
    std::lock_guard<std::mutex> l(G().m);
    G().maxConcurrent = n < 1 ? 1 : n;
}

int HttpClient::ActiveCount() {
    std::lock_guard<std::mutex> l(G().m);
    int n = 0;
    for (size_t i = 0; i < G().all.size(); ++i) {
        std::shared_ptr<CallImpl> c = G().all[i].lock();
        if (c && !c->IsDone() && c->Started()) ++n;
    }
    return n;
}

int HttpClient::QueuedCount() {
    std::lock_guard<std::mutex> l(G().m);
    return (int)G().queue.size();
}

std::shared_ptr<HttpCall> HttpClient::SendImpl(const HttpRequest& req,
        std::function<bool()> alive, std::function<bool(const HttpResult&)> deliver,
        std::shared_ptr<Channel<HttpResult> > alsoPush) {
    std::shared_ptr<CallImpl> c(new CallImpl(req, alive, deliver, alsoPush));
    HttpResult err;
    bool valid = c->Prepare(err);
    bool start = false;
    {
        std::lock_guard<std::mutex> l(G().m);
        if (!G().running) return std::shared_ptr<HttpCall>();
        for (size_t i = 0; i < G().all.size();) {
            if (G().all[i].expired()) G().all.erase(G().all.begin() + i);
            else ++i;
        }
        G().all.push_back(c);
        if (valid) {
            if (c->IsOwn()) {
                c->MarkStarted(); // Own은 상한 큐를 거치지 않는다
                start = true;
            } else if (G().active < G().maxConcurrent) {
                ++G().active;
                c->MarkStarted();
                start = true;
            } else {
                c->SetQueued(true);
                G().queue.push_back(c);
            }
        }
    }
    if (!valid) c->Abort(err.error, err.message); // 결과는 UI 스레드로 전달된다
    else if (start) c->Begin();
    return c;
}

} // namespace nfc
