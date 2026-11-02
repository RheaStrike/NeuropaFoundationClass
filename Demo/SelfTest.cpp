// 회귀 테스트 (--selftest). 기존 main.cpp의 검증 항목을 그대로 옮겼다.
#include "framework.h"
#include "SelfTest.h"

#ifdef NFC_CUI
#include "App.h"
#include "Dialog.h"
#include "Label.h"
#include "Button.h"
#include "NinePatch.h"
#include "CString.h"
#include "Channel.h"
#include "Socket.h"
#include "HttpClient.h"
#include "DialogTemplate.h"
#include "resource.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <future>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
void Trace(int n) { if (GetEnvironmentVariableW(L"NFC_TRACE", NULL, 0)) { wprintf(L"[t%d]\n", n); fflush(stdout); } }
// 워커 쪽 비동기 객체 예: 자기 직렬 큐(strand)에서만 상태를 만진다 → 잠금 불필요
class Recorder : public nfc::CObject {
public:
    explicit Recorder(std::promise<void>* died = NULL) : m_inside(0), m_overlap(false),
        m_died(died) {}
    ~Recorder() { if (m_died) m_died->set_value(); }
    void Add(int v) {
        if (++m_inside != 1) m_overlap = true; // 동시에 둘이 돌면 직렬화 실패
        m_values.push_back(v);
        std::this_thread::yield();
        --m_inside;
    }
    std::vector<int> m_values;
    std::atomic<int> m_inside;
    std::atomic<bool> m_overlap;
private:
    std::promise<void>* m_died;
};

bool Fail(const wchar_t* what) {
    wprintf(L"selftest: %ls\n", what);
    return false;
}

// CObject 직렬 큐: 순서 + 직렬화 + 수명 + 거절 + Control::RunAsync (창 없이)
bool TestTasks() {
    // 1) 한 스레드에서 넣은 순서 그대로, 겹치지 않고
    { Trace(1);
        std::shared_ptr<Recorder> r(new Recorder());
        for (int i = 0; i < 1000; ++i) {
            Recorder* p = r.get();
            if (!r->Post([p, i]() { p->Add(i); })) return Fail(L"post rejected");
        }
        Recorder* p = r.get();
        std::future<size_t> n = r->RunAsync([p]() { return p->m_values.size(); });
        if (n.get() != 1000) return Fail(L"strand count");
        for (int i = 0; i < 1000; ++i)
            if (r->m_values[i] != i) return Fail(L"strand order");
        if (r->m_overlap) return Fail(L"strand overlap");
    }
    // 2) 여러 스레드에서 동시에 넣어도 직렬, 스레드별 순서 유지
    { Trace(2);
        std::shared_ptr<Recorder> r(new Recorder());
        std::vector<std::thread> ths;
        for (int t = 0; t < 4; ++t) {
            ths.push_back(std::thread([r, t]() {
                Recorder* p = r.get();
                for (int i = 0; i < 250; ++i) r->Post([p, t, i]() { p->Add(t * 1000 + i); });
            }));
        }
        for (size_t i = 0; i < ths.size(); ++i) ths[i].join();
        Recorder* p = r.get();
        std::future<void> done = r->RunAsync([p]() {});
        done.get();
        if (r->m_values.size() != 1000) return Fail(L"multi count");
        if (r->m_overlap) return Fail(L"multi overlap");
        int last[4] = { -1, -1, -1, -1 };
        for (size_t i = 0; i < r->m_values.size(); ++i) {
            int t = r->m_values[i] / 1000, v = r->m_values[i] % 1000;
            if (v <= last[t]) return Fail(L"multi order");
            last[t] = v;
        }
    }
    // 3) 큐에 작업이 있는 동안은 마지막 shared_ptr를 놓아도 객체가 살아 있다
    { Trace(3);
        std::promise<void> died;
        std::future<void> diedF = died.get_future();
        std::promise<void> gate;
        std::shared_future<void> gateF = gate.get_future().share();
        std::shared_ptr<Recorder> r(new Recorder(&died));
        std::weak_ptr<Recorder> w = r;
        r->Post([gateF]() { gateF.wait(); }); // 문이 열릴 때까지 큐를 막는다
        Recorder* p = r.get();
        std::future<int> v = r->RunAsync([p]() { p->Add(7); return 7; });
        r.reset();
        if (w.expired()) return Fail(L"lifetime: died while queued");
        gate.set_value();
        if (v.get() != 7) return Fail(L"lifetime: result");
        if (diedF.wait_for(std::chrono::seconds(5)) != std::future_status::ready)
            return Fail(L"lifetime: not released");
    }
    // 4) shared_ptr로 소유되지 않은 객체는 거절 (스택 객체도 안전)
    { Trace(4);
        Recorder stackObj;
        if (stackObj.IsShared()) return Fail(L"stack IsShared");
        if (stackObj.Post([]() {})) return Fail(L"stack post accepted");
        std::future<int> f = stackObj.RunAsync([]() { return 1; });
        bool threw = false;
        try { f.get(); } catch (const nfc::NfcException&) { threw = true; }
        if (!threw) return Fail(L"stack RunAsync not rejected");
    }
    // 5) 작업 예외는 future로 전달
    { Trace(5);
        std::shared_ptr<Recorder> r(new Recorder());
        std::future<int> f = r->RunAsync([]() -> int { throw std::runtime_error("x"); });
        bool threw = false;
        try { f.get(); } catch (const std::runtime_error&) { threw = true; }
        if (!threw) return Fail(L"exception not propagated");
    }
    // 6) Control::RunAsync: 워커에서 일하고 UI 스레드(이 스레드)에서 완료 수신
    { Trace(6);
        std::shared_ptr<nfc::Label> lab(new nfc::Label());
        int got = 0;
        bool onUi = false;
        bool ok = lab->RunAsync([]() { return 40 + 2; },
            [&got, &onUi](std::shared_future<int> f) {
                got = f.get();
                onUi = nfc::App::IsUiThread();
                nfc::App::Quit(0);
            });
        if (!ok) return Fail(L"control RunAsync rejected");
        nfc::App::Run(); // 메시지 전용 알림 창으로 PostToUI가 온다 (창 없음)
        if (got != 42 || !onUi) return Fail(L"control RunAsync result");
        nfc::Label stackLab;
        if (stackLab.RunAsync([]() { return 0; }, [](std::shared_future<int>) {}))
            return Fail(L"stack control RunAsync accepted");
    }
    return true;
}

// nfc::CString 단위 테스트 (MFC CString 호환 동작)
bool TestCString() {
    typedef nfc::CString S;
    S a(L"Hello"), b = L"World";
    if (a.GetLength() != 5 || a.IsEmpty() || !S().IsEmpty()) return Fail(L"cstr length");
    if (S(L'x', 3) != L"xxx") return Fail(L"cstr repeat");
    if (a + L", " + b != L"Hello, World") return Fail(L"cstr concat");
    S c = a; c += L'!'; if (c != L"Hello!" || a != L"Hello") return Fail(L"cstr copy/+=");
    if (!(a < b) || !(b > a) || !(a <= a) || !(a >= a) || a == b) return Fail(L"cstr compare ops");
    if (a.Compare(L"Hello") != 0 || a.CompareNoCase(L"hELLO") != 0 || a.Compare(L"Z") >= 0)
        return Fail(L"cstr Compare");
    if (a[1] != L'e' || a.GetAt(4) != L'o') return Fail(L"cstr GetAt");
    c.SetAt(0, L'J'); if (c != L"Jello!") return Fail(L"cstr SetAt");
    const wchar_t* raw = a; if (wcscmp(raw, L"Hello") != 0 || wcscmp(a.GetString(), L"Hello"))
        return Fail(L"cstr LPCTSTR");
    if (a.Left(2) != L"He" || a.Right(3) != L"llo" || a.Mid(1) != L"ello" ||
            a.Mid(1, 2) != L"el" || a.Mid(10) != L"" || a.Left(99) != L"Hello")
        return Fail(L"cstr Left/Right/Mid");
    if (a.Find(L'l') != 2 || a.Find(L'l', 3) != 3 || a.Find(L'z') != -1 ||
            a.Find(L"llo") != 2 || a.Find(L"xyz") != -1 || a.Find(L'H', 99) != -1)
        return Fail(L"cstr Find");
    if (a.ReverseFind(L'l') != 3 || a.ReverseFind(L'q') != -1) return Fail(L"cstr ReverseFind");
    if (a.FindOneOf(L"xyo") != 4 || a.FindOneOf(L"qz") != -1) return Fail(L"cstr FindOneOf");
    S r = L"a-b-c-d";
    if (r.Replace(L'-', L'+') != 3 || r != L"a+b+c+d") return Fail(L"cstr Replace ch");
    if (r.Replace(L"+", L"::") != 3 || r != L"a::b::c::d") return Fail(L"cstr Replace str");
    if (r.Replace(L"zz", L"y") != 0) return Fail(L"cstr Replace none");
    if (r.Remove(L':') != 6 || r != L"abcd") return Fail(L"cstr Remove");
    r.Insert(2, L'X'); r.Insert(0, L"<<"); r.Insert(99, L">");
    if (r != L"<<abXcd>") return Fail(L"cstr Insert");
    r.Delete(0, 2); r.Delete(2); if (r != L"abcd>") return Fail(L"cstr Delete");
    S t = L"  \t trim me \r\n";
    t.Trim(); if (t != L"trim me") return Fail(L"cstr Trim");
    t = L"xxhixx"; t.TrimLeft(L'x'); if (t != L"hixx") return Fail(L"cstr TrimLeft ch");
    t.TrimRight(L"x"); if (t != L"hi") return Fail(L"cstr TrimRight set");
    t = L"MiXeD"; if (S(t).MakeUpper() != L"MIXED" || S(t).MakeLower() != L"mixed" ||
            S(t).MakeReverse() != L"DeXiM") return Fail(L"cstr Make*");
    S sp = L"123abc";
    if (sp.SpanIncluding(L"0123456789") != L"123" || sp.SpanExcluding(L"c") != L"123ab")
        return Fail(L"cstr Span");
    S tok = L"%a, b,,c%"; int pos = 0; S parts;
    S x = tok.Tokenize(L"%, ", pos);
    while (pos != -1) { parts += x; parts += L'|'; x = tok.Tokenize(L"%, ", pos); }
    if (parts != L"a|b|c|") return Fail(L"cstr Tokenize");
    S f; f.Format(L"%d-%s-%.2f", 42, L"x", 1.5);
    if (f != L"42-x-1.50") return Fail(L"cstr Format");
    f.AppendFormat(L"/%04X", 255); if (f != L"42-x-1.50/00FF") return Fail(L"cstr AppendFormat");
    S big; big.Format(L"%s%s", std::wstring(3000, L'a').c_str(), L"b");
    if (big.GetLength() != 3001 || big.Right(1) != L"b") return Fail(L"cstr Format long");
    S buf; wchar_t* pb = buf.GetBuffer(16); wcscpy_s(pb, 16, L"abc"); buf.ReleaseBuffer();
    if (buf.GetLength() != 3 || buf != L"abc") return Fail(L"cstr GetBuffer");
    pb = buf.GetBufferSetLength(2); pb[1] = L'Z'; buf.ReleaseBuffer(2);
    if (buf != L"aZ") return Fail(L"cstr GetBufferSetLength");
    buf.FreeExtra();
    S ap; ap.Append(L"ab"); ap.AppendChar(L'c'); ap.Append(L"defg", 2); ap.Truncate(4);
    if (ap != L"abcd") return Fail(L"cstr Append/Truncate");
    S n8("narrow"); if (n8 != L"narrow" || n8.ToString() != "narrow" ||
            n8.ToWString() != L"narrow") return Fail(L"cstr std conv");
    S ko = L"\uD55C\uAE00"; if (ko.ToUtf8() != "\xED\x95\x9C\xEA\xB8\x80") return Fail(L"cstr utf8");
    if (S(L"123").ToInt() != 123 || S::FromInt(-7) != L"-7" || S(L"2.5").ToDouble() != 2.5)
        return Fail(L"cstr number");
    S ls; if (ls.LoadString(GetModuleHandle(NULL), 65000) || !ls.IsEmpty())
        return Fail(L"cstr LoadString missing");
    a.Empty(); if (!a.IsEmpty()) return Fail(L"cstr Empty");
    return true;
}

// CUI용 UI 메시지 펌프: done()이 참이 되거나 timeoutMs가 지날 때까지 PostToUI 작업을 처리.
// timeoutMs가 지나도 done이 거짓이면 false. (창 없음, 알림 창 메시지만 돈다)
bool PumpUI(std::function<bool()> done, DWORD timeoutMs) {
    ULONGLONG end = GetTickCount64() + timeoutMs;
    for (;;) {
        MSG msg;
        while (PeekMessage(&msg, NULL, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessage(&msg);
        }
        if (done && done()) return true;
        ULONGLONG now = GetTickCount64();
        if (now >= end) return !done;
        // 작업 스레드가 바꾼 조건도 보도록 최대 10ms마다 깨어난다
        DWORD wait = (DWORD)(end - now);
        MsgWaitForMultipleObjects(0, NULL, FALSE, wait < 10 ? wait : 10, QS_ALLINPUT);
    }
}

// 지금 큐에 들어온 UI 작업만 처리 (잠깐 기다린 뒤 비우기)
void PumpPending() { PumpUI(std::function<bool()>(), 50); }

// 채널: 구독 전 쌓임/Drain, 해제 후 쌓임, 다중 스레드 Push, 전달 중 해제, 채널 먼저 소멸, 한도
bool TestChannel() {
    typedef nfc::Channel<int> Ch;
    // 1) 구독 전 쌓임 → 구독하면 순서대로 Drain
    { Trace(7);
        std::shared_ptr<Ch> ch = Ch::Create();
        for (int i = 0; i < 3; ++i) ch->Push(i);
        if (ch->Pending() != 3) return Fail(L"chan pending before sub");
        std::vector<int> got;
        nfc::Subscription sub = ch->Subscribe([&got](const int& v) { got.push_back(v); });
        if (!got.empty()) return Fail(L"chan delivered synchronously");
        if (!PumpUI([&got]() { return got.size() == 3; }, 3000)) return Fail(L"chan drain");
        if (got[0] != 0 || got[1] != 1 || got[2] != 2 || ch->Pending() != 0)
            return Fail(L"chan drain order");
        // 2) 해제 후에는 쌓이기만 하고, 다시 구독하면 이어서 전달
        sub.Cancel();
        if (ch->SubscriberCount() != 0) return Fail(L"chan cancel count");
        ch->Push(3);
        ch->Push(4);
        PumpPending();
        if (got.size() != 3 || ch->Pending() != 2) return Fail(L"chan after cancel");
        nfc::Subscription sub2 = ch->Subscribe([&got](const int& v) { got.push_back(v); });
        if (!PumpUI([&got]() { return got.size() == 5; }, 3000) || got[3] != 3 || got[4] != 4)
            return Fail(L"chan resubscribe");
        // 3) 이미 PostToUI에 들어간 전달도 실행 전에 해제되면 건너뛴다 (메시지는 남는다)
        ch->Push(5);
        sub2.Cancel();
        PumpPending();
        if (got.size() != 5 || ch->Pending() != 1) return Fail(L"chan cancel before run");
    }
    // 4) 여러 스레드에서 Push: 개수와 스레드별 순서 보장
    { Trace(8);
        std::shared_ptr<Ch> ch = Ch::Create();
        std::vector<int> got;
        nfc::Subscription sub = ch->Subscribe([&got](const int& v) { got.push_back(v); });
        std::vector<std::thread> ths;
        for (int t = 0; t < 4; ++t) {
            ths.push_back(std::thread([ch, t]() {
                for (int i = 0; i < 250; ++i) ch->Push(t * 1000 + i);
            }));
        }
        for (size_t i = 0; i < ths.size(); ++i) ths[i].join();
        if (!PumpUI([&got]() { return got.size() == 1000; }, 5000)) return Fail(L"chan mt count");
        int last[4] = { -1, -1, -1, -1 };
        for (size_t i = 0; i < got.size(); ++i) {
            int t = got[i] / 1000, v = got[i] % 1000;
            if (v <= last[t]) return Fail(L"chan mt order");
            last[t] = v;
        }
    }
    // 5) 전달 중 해제(자기 자신/다른 구독자)와 재진입(핸들러 안 Push) 안전
    { Trace(9);
        std::shared_ptr<Ch> ch = Ch::Create();
        int a = 0, b = 0, c = 0;
        nfc::Subscription subA, subB, subC;
        subA = ch->Subscribe([&](const int&) { ++a; subA.Cancel(); }); // 첫 메시지 후 자기 해제
        subB = ch->Subscribe([&](const int& v) {
            ++b;
            if (v == 2) subC.Cancel(); // 같은 메시지 전달 도중 뒤 구독자 해제
            if (v == 4) ch->Push(99);  // 재진입 Push
        });
        subC = ch->Subscribe([&](const int&) { ++c; });
        for (int i = 0; i < 5; ++i) ch->Push(i);
        if (!PumpUI([&b]() { return b == 6; }, 3000)) return Fail(L"chan reentrant");
        if (a != 1 || c != 2) return Fail(L"chan cancel during delivery");
    }
    // 6) 채널이 구독자보다 먼저 사라져도 안전 (예약된 전달·토큰 해제 모두)
    { Trace(10);
        int n = 0;
        nfc::Subscription sub;
        {
            std::shared_ptr<Ch> ch = Ch::Create();
            sub = ch->Subscribe([&n](const int&) { ++n; });
            ch->Push(1); // 전달이 예약된 채로 채널 소멸
        }
        PumpPending();
        if (n != 0) return Fail(L"chan dead channel delivered");
        sub.Cancel(); // 죽은 채널 해제도 안전
        if (sub.IsActive()) return Fail(L"chan token active");
    }
    // 7) weak 대상: 대상이 죽으면 건너뛰고 구독 종료
    { Trace(11);
        std::shared_ptr<Ch> ch = Ch::Create();
        std::shared_ptr<std::vector<int> > target(new std::vector<int>());
        std::weak_ptr<std::vector<int> > wt = target;
        nfc::Subscription sub = ch->Subscribe<std::vector<int> >(target,
            [](std::vector<int>& t, const int& v) { t.push_back(v); });
        ch->Push(1);
        if (!PumpUI([wt]() { std::shared_ptr<std::vector<int> > t = wt.lock();
                return t && t->size() == 1; }, 3000)) return Fail(L"chan weak deliver");
        target.reset();
        ch->Push(2);
        PumpPending();
        if (ch->SubscriberCount() != 0) return Fail(L"chan weak target not dropped");
        if (ch->Pending() != 0) return Fail(L"chan weak target pending"); // 꺼낸 뒤 대상 소멸 확인
    }
    // 8) 한도: DropOldest는 오래된 것 버림, Reject는 새 것 거부
    { Trace(12);
        std::shared_ptr<Ch> ch = Ch::Create(3, Ch::DropOldest);
        for (int i = 0; i < 5; ++i) ch->Push(i);
        if (ch->Pending() != 3 || ch->Dropped() != 2) return Fail(L"chan drop oldest");
        std::vector<int> got;
        nfc::Subscription sub = ch->Subscribe([&got](const int& v) { got.push_back(v); });
        if (!PumpUI([&got]() { return got.size() == 3; }, 3000) ||
                got[0] != 2 || got[1] != 3 || got[2] != 4) return Fail(L"chan drop oldest order");
        sub.Cancel();
        std::shared_ptr<Ch> rj = Ch::Create(3, Ch::Reject);
        int accepted = 0;
        for (int i = 0; i < 5; ++i) if (rj->Push(i)) ++accepted;
        if (accepted != 3 || rj->Rejected() != 2 || rj->Pending() != 3) return Fail(L"chan reject");
    }
    // 9) Dialog 헬퍼: 창이 닫히면(여기선 Dialog 소멸) 구독 해제 (창 없이 CUI에서)
    { Trace(13);
        std::shared_ptr<Ch> ch = Ch::Create();
        {
            nfc::Dialog d;
            d.Subscribe<int>(ch, [](const int&) {});
            if (ch->SubscriberCount() != 1) return Fail(L"chan dialog subscribe");
        }
        if (ch->SubscriberCount() != 0) return Fail(L"chan dialog auto cancel");
        PumpPending();
    }
    return true;
}

// ===================== 네트워크 테스트 (loopback만) =====================
struct Sink { // 소유자 역할 (컨트롤 대신)
    std::string data;
    std::vector<int> events;
    int Count(int t) const { int n = 0; for (size_t i = 0; i < events.size(); ++i) if (events[i] == t) ++n; return n; }
};

std::shared_ptr<nfc::Socket> MakeClient(nfc::Mode mode, const std::shared_ptr<Sink>& sink) {
    std::shared_ptr<nfc::Socket> s = nfc::Socket::Create(mode);
    if (!s) return s;
    s->SetHandlers<Sink>(sink,
        [](Sink& k, const nfc::Bytes& b) { k.data.append(b.begin(), b.end()); },
        [](Sink& k, const nfc::NetEvent& e) { k.events.push_back(e.type); });
    return s;
}

std::shared_ptr<nfc::TcpListener> StartEcho() {
    return nfc::TcpListener::Create(0, true, [](const std::shared_ptr<nfc::Socket>& s) {
        s->SetIoReceiveHandler([](nfc::Socket& self, const nfc::Bytes& b) { self.Send(b); });
    });
}

bool TestTimer() { // RunAfter 기본 동작
    std::shared_ptr<std::atomic<int> > hit(new std::atomic<int>(0));
    if (!nfc::NetworkManager::RunAfter(100, [hit]() { ++*hit; })) return Fail(L"timer start");
    bool ok = PumpUI([hit]() { return hit->load() > 0; }, 3000);
    return ok ? true : Fail(L"timer");
}

bool TestSockets() {
    if (!TestTimer()) return false;
    std::shared_ptr<nfc::TcpListener> echo = StartEcho();
    if (!echo || echo->Port() == 0) return Fail(L"net listener");
    // 1) 에코 왕복 (Delegate / Own 두 모드)
    for (int m = 0; m < 2; ++m) { Trace(14);
        nfc::Mode mode = m == 0 ? nfc::Mode::Delegate : nfc::Mode::Own;
        std::shared_ptr<Sink> k(new Sink());
        std::shared_ptr<nfc::Socket> s = MakeClient(mode, k);
        if (!s || s->GetMode() != mode) return Fail(L"net create");
        if (!s->Send(std::string("ping"))) return Fail(L"net send before connect"); // 연결 전 큐잉
        if (!s->Connect("127.0.0.1", echo->Port())) return Fail(L"net connect call");
        if (!PumpUI([k]() { return k->data == "ping"; }, 5000)) return Fail(L"net echo roundtrip");
        if (k->events.empty() || k->events[0] != nfc::NET_CONNECTED) return Fail(L"net connected event");
        s->Close();
        s->Close();
        if (!PumpUI([k]() { return k->Count(nfc::NET_DISCONNECTED) == 1; }, 3000))
            return Fail(L"net disconnected event");
    }
    // 2) 대량 송신 순서 보장
    { Trace(15);
        std::shared_ptr<Sink> k(new Sink());
        std::shared_ptr<nfc::Socket> s = MakeClient(nfc::Mode::Delegate, k);
        s->Connect("127.0.0.1", echo->Port());
        std::string expect;
        for (int i = 0; i < 2000; ++i) {
            char b[16];
            sprintf_s(b, "%05d;", i);
            expect += b;
            if (!s->Send(std::string(b))) return Fail(L"net bulk send");
        }
        if (!PumpUI([k, &expect]() { return k->data.size() >= expect.size(); }, 10000) ||
                k->data != expect) return Fail(L"net bulk order");
        s->Close();
    }
    // 3) 연결 거부 / 잘못된 포트 → 오류 이벤트
    { Trace(16);
        std::shared_ptr<nfc::TcpListener> tmp = nfc::TcpListener::Create(0, true);
        unsigned short dead = tmp->Port();
        tmp->Close();
        tmp.reset();
        PumpUI(std::function<bool()>(), 200);
        std::shared_ptr<Sink> k(new Sink());
        std::shared_ptr<nfc::Socket> s = MakeClient(nfc::Mode::Delegate, k);
        s->Connect("127.0.0.1", dead);
        if (!PumpUI([k]() { return k->Count(nfc::NET_ERROR) == 1; }, 15000)) return Fail(L"net refused");
        if (!s->IsClosed()) return Fail(L"net refused not closed");
        std::shared_ptr<Sink> k2(new Sink());
        std::shared_ptr<nfc::Socket> s2 = MakeClient(nfc::Mode::Delegate, k2);
        s2->Connect("127.0.0.1", 0);
        if (!PumpUI([k2]() { return k2->Count(nfc::NET_ERROR) == 1; }, 15000)) return Fail(L"net bad port");
    }
    // 4) Close 중복·다른 스레드 호출 안전 (Disconnected는 한 번)
    { Trace(17);
        std::shared_ptr<Sink> k(new Sink());
        std::shared_ptr<nfc::Socket> s = MakeClient(nfc::Mode::Delegate, k);
        s->Connect("127.0.0.1", echo->Port());
        if (!PumpUI([k]() { return k->Count(nfc::NET_CONNECTED) == 1; }, 5000)) return Fail(L"net close connect");
        std::vector<std::thread> ths;
        for (int t = 0; t < 4; ++t) ths.push_back(std::thread([s]() { for (int i = 0; i < 10; ++i) s->Close(); }));
        s->Close();
        for (size_t i = 0; i < ths.size(); ++i) ths[i].join();
        PumpUI(std::function<bool()>(), 300);
        if (!s->IsClosed() || k->Count(nfc::NET_DISCONNECTED) != 1) return Fail(L"net close idempotent");
        if (s->Send(std::string("x"))) return Fail(L"net send after close");
    }
    // 5) 소유자가 사라진 뒤 데이터가 와도 안전 (버리고 닫는다)
    { Trace(18);
        std::shared_ptr<Sink> k(new Sink());
        std::shared_ptr<nfc::Socket> s = MakeClient(nfc::Mode::Delegate, k);
        s->Connect("127.0.0.1", echo->Port());
        if (!PumpUI([k]() { return k->Count(nfc::NET_CONNECTED) == 1; }, 5000)) return Fail(L"net owner connect");
        k.reset();
        s->Send(std::string("late"));
        if (!PumpUI([s]() { return s->IsClosed(); }, 5000)) return Fail(L"net owner gone close");
    }
    // 6) 전송 큐 한도
    { Trace(19);
        std::shared_ptr<nfc::Socket> s = nfc::Socket::Create();
        s->SetMaxSendQueue(100);
        if (!s->Send(std::string(60, 'a'))) return Fail(L"net queue first");
        if (s->Send(std::string(60, 'b'))) return Fail(L"net queue limit");
        if (s->QueuedBytes() != 60) return Fail(L"net queue bytes");
        s->Close();
    }
    // 7) 핸들러 예외 격리: 터진 소켓만 닫히고 나머지는 계속 동작
    { Trace(20);
        long before = nfc::NetworkManager::GetStats().exceptions;
        std::shared_ptr<nfc::TcpListener> bomb = nfc::TcpListener::Create(0, true,
            [](const std::shared_ptr<nfc::Socket>& s) {
                s->SetIoReceiveHandler([](nfc::Socket&, const nfc::Bytes&) {
                    throw std::runtime_error("boom");
                });
            });
        std::shared_ptr<Sink> ka(new Sink());
        std::shared_ptr<nfc::Socket> a = MakeClient(nfc::Mode::Delegate, ka);
        a->Connect("127.0.0.1", bomb->Port());
        a->Send(std::string("x"));
        if (!PumpUI([ka]() { return ka->Count(nfc::NET_DISCONNECTED) + ka->Count(nfc::NET_ERROR) >= 1; }, 5000))
            return Fail(L"net isolate peer closed");
        if (nfc::NetworkManager::GetStats().exceptions <= before) return Fail(L"net isolate counter");
        std::shared_ptr<Sink> kb(new Sink());
        std::shared_ptr<nfc::Socket> b = MakeClient(nfc::Mode::Delegate, kb);
        b->Connect("127.0.0.1", echo->Port());
        b->Send(std::string("alive"));
        if (!PumpUI([kb]() { return kb->data == "alive"; }, 5000)) return Fail(L"net isolate others");
        b->Close();
        bomb->Close();
    }
    // 8) Own 소멸 안전 + 다른 스레드에서 마지막 참조 해제
    { Trace(21);
        std::future<bool> f = std::async(std::launch::async, [&echo]() {
            for (int i = 0; i < 5; ++i) {
                std::shared_ptr<nfc::Socket> s = nfc::Socket::Create(nfc::Mode::Own);
                if (!s) return false;
                s->Connect("127.0.0.1", echo->Port());
                s->Send(std::string("bye"));
                // 여기(워커 스레드)에서 마지막 참조 해제 → stop+join
            }
            std::shared_ptr<nfc::Socket> d = nfc::Socket::Create(nfc::Mode::Delegate);
            d->Connect("127.0.0.1", echo->Port());
            return true;
        });
        if (f.wait_for(std::chrono::seconds(10)) != std::future_status::ready || !f.get())
            return Fail(L"net own destroy");
        PumpPending();
    }
    echo->Close();
    return true;
}

// 최소 HTTP 테스트 서버 (평문, loopback). 경로별 동작:
// /hello, POST /echo, /big(100000B), /slow(3초 지연), /redir/N, /hdr(큰 헤더)
void HttpRespond(nfc::Socket& s, int code, const std::string& body, const std::string& extra) {
    std::string r = "HTTP/1.1 " + std::to_string(code) + (code == 200 ? " OK" : " Found") +
        "\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n" +
        extra + "\r\n" + body;
    s.Send(r);
    s.CloseWhenSent();
}

std::shared_ptr<nfc::TcpListener> StartHttpServer() {
    return nfc::TcpListener::Create(0, true, [](const std::shared_ptr<nfc::Socket>& sock) {
        std::shared_ptr<std::string> buf(new std::string());
        std::weak_ptr<nfc::Socket> ws = sock;
        sock->SetIoReceiveHandler([buf, ws](nfc::Socket& s, const nfc::Bytes& b) {
            buf->append(b.begin(), b.end());
            size_t he = buf->find("\r\n\r\n");
            if (he == std::string::npos) return;
            size_t clen = 0;
            std::string head = buf->substr(0, he);
            std::string lower = head;
            for (size_t i = 0; i < lower.size(); ++i) lower[i] = (char)tolower((unsigned char)lower[i]);
            size_t cl = lower.find("content-length:");
            if (cl != std::string::npos) clen = (size_t)atoi(head.c_str() + cl + 15);
            if (buf->size() < he + 4 + clen) return;
            std::string body = buf->substr(he + 4, clen);
            size_t sp1 = head.find(' '), sp2 = head.find(' ', sp1 + 1);
            std::string method = head.substr(0, sp1), path = head.substr(sp1 + 1, sp2 - sp1 - 1);
            buf->clear();
            if (path == "/hello") HttpRespond(s, 200, "hello", "");
            else if (path == "/echo" && method == "POST") HttpRespond(s, 200, body, "");
            else if (path == "/big") HttpRespond(s, 200, std::string(100000, 'x'), "");
            else if (path == "/hdr") HttpRespond(s, 200, "h", "X-Big: " + std::string(8000, 'y') + "\r\n");
            else if (path == "/slow") {
                nfc::NetworkManager::RunAfter(3000, [ws]() {
                    std::shared_ptr<nfc::Socket> p = ws.lock();
                    if (p) HttpRespond(*p, 200, "slow", "");
                });
            } else if (path.compare(0, 7, "/redir/") == 0) {
                int n = atoi(path.c_str() + 7);
                if (n <= 0) HttpRespond(s, 200, "done", "");
                else HttpRespond(s, 302, "", "Location: /redir/" + std::to_string(n - 1) + "\r\n");
            } else HttpRespond(s, 404, "", "");
        });
    });
}

struct HttpSink {
    HttpSink() : calls(0) {}
    int calls;
    nfc::HttpResult last;
};

std::shared_ptr<nfc::HttpCall> Http(const std::shared_ptr<HttpSink>& k, nfc::HttpRequest req) {
    return nfc::HttpClient::Send<HttpSink>(req, k,
        [](HttpSink& s, const nfc::HttpResult& r) { ++s.calls; s.last = r; });
}

nfc::HttpRequest Req(unsigned short port, const char* path, nfc::Mode mode = nfc::Mode::Delegate) {
    nfc::HttpRequest r;
    r.url = L"http://127.0.0.1:" + std::to_wstring(port) + nfc::CString(path).str();
    r.mode = mode;
    return r;
}

bool WaitHttp(const std::shared_ptr<HttpSink>& k, int calls, DWORD ms) {
    Trace(1000 + calls);
    DWORD t0 = GetTickCount();
    bool ok = PumpUI([k, calls]() { return k->calls >= calls; }, ms);
    if (GetEnvironmentVariableW(L"NFC_TRACE", NULL, 0))
        wprintf(L"  calls=%d error=%d sys=%lu status=%d ms=%lu body=%hs msg=%hs\n", k->calls, k->last.error,
            (unsigned long)k->last.systemError, k->last.status, GetTickCount() - t0,
            k->last.BodyText().substr(0, 20).c_str(), k->last.message.c_str());
    return ok;
}

bool TestHttpUnits() {
    nfc::HttpUrl u;
    if (!nfc::HttpUrl::Parse(L"https://example.com/a/b?x=1#frag", u) || !u.secure ||
            u.host != L"example.com" || u.port != 443 || u.path != L"/a/b?x=1") return Fail(L"url https");
    if (!nfc::HttpUrl::Parse(L"http://127.0.0.1:8080", u) || u.secure || u.port != 8080 || u.path != L"/")
        return Fail(L"url port");
    if (!nfc::HttpUrl::Parse(L"http://[::1]:81/p", u) || u.host != L"::1" || u.port != 81)
        return Fail(L"url ipv6");
    if (!nfc::HttpUrl::Parse(L"HTTP://h?q", u) || u.path != L"/?q") return Fail(L"url query only");
    const wchar_t* bad[] = { L"ftp://h/", L"http://", L"http://user:pw@h/", L"http://h:0/",
        L"http://h:70000/", L"http://h:/", L"h/path", L"http://h/a b", L"http://h/\r\nX: y" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i)
        if (nfc::HttpUrl::Parse(bad[i], u)) return Fail(L"url bad accepted");
    nfc::HttpUrl base, out;
    nfc::HttpUrl::Parse(L"https://a.com/x/y?z", base);
    if (!nfc::HttpClient::ResolveRedirect(base, L"/r", out) || out.ToString() != L"https://a.com/r")
        return Fail(L"redirect abs path");
    if (!nfc::HttpClient::ResolveRedirect(base, L"q", out) || out.ToString() != L"https://a.com/x/q")
        return Fail(L"redirect rel");
    if (!nfc::HttpClient::ResolveRedirect(base, L"//b.com/p", out) || out.ToString() != L"https://b.com/p")
        return Fail(L"redirect scheme rel");
    nfc::HttpUrl::Parse(L"http://a.com/", out);
    if (nfc::HttpClient::IsRedirectAllowed(base, out)) return Fail(L"redirect downgrade allowed");
    if (!nfc::HttpClient::IsRedirectAllowed(out, base)) return Fail(L"redirect upgrade blocked");
    nfc::HttpSecurityOptions so = nfc::HttpClient::SecurityOptions();
    if (!so.certificateValidation || !so.tls12Minimum || so.autoRedirect || so.cookies)
        return Fail(L"http security options");
    if (nfc::HttpClient::RedactHeaderForLog(L"Authorization", L"Bearer s3cret") != L"***" ||
            nfc::HttpClient::RedactHeaderForLog(L"cookie", L"a=b") != L"***" ||
            nfc::HttpClient::RedactHeaderForLog(L"Accept", L"*/*") != L"*/*") return Fail(L"http redact");
    nfc::HttpRequest d;
    if (d.connectTimeoutMs != 10000 || d.sendTimeoutMs != 30000 || d.receiveTimeoutMs != 30000 ||
            d.maxBodyBytes != 16u * 1024u * 1024u || d.maxRedirects != 5 || d.mode != nfc::Mode::Delegate)
        return Fail(L"http defaults");
    return true;
}

bool TestHttp() {
    if (!TestHttpUnits()) return false;
    std::shared_ptr<nfc::TcpListener> srv = StartHttpServer();
    if (!srv) return Fail(L"http server");
    unsigned short port = srv->Port();
    // 1) GET / POST 왕복 (Delegate, Own)
    for (int m = 0; m < 2; ++m) { Trace(22);
        nfc::Mode mode = m == 0 ? nfc::Mode::Delegate : nfc::Mode::Own;
        std::shared_ptr<HttpSink> k(new HttpSink());
        Http(k, Req(port, "/hello", mode));
        if (!WaitHttp(k, 1, 10000) || !k->last.Ok() || k->last.status != 200 || k->last.BodyText() != "hello")
            return Fail(L"http get");
        nfc::HttpRequest p = Req(port, "/echo", mode);
        p.method = L"POST";
        std::string body = "abc123";
        p.body.assign(body.begin(), body.end());
        p.headers.push_back(nfc::HttpHeader());
        p.headers.back().name = L"Content-Type";
        p.headers.back().value = L"text/plain";
        Http(k, p);
        if (!WaitHttp(k, 2, 10000) || k->last.BodyText() != body) return Fail(L"http post");
        // 2) 본문 한도 초과
        nfc::HttpRequest big = Req(port, "/big", mode);
        big.maxBodyBytes = 1000;
        Http(k, big);
        if (!WaitHttp(k, 3, 10000) || k->last.error != nfc::HTTP_ERR_TOO_LARGE) return Fail(L"http too large");
        // 3) 느린 응답 → 수신 타임아웃
        nfc::HttpRequest slow = Req(port, "/slow", mode);
        slow.receiveTimeoutMs = 500;
        Http(k, slow);
        if (!WaitHttp(k, 4, 10000) || k->last.error != nfc::HTTP_ERR_TIMEOUT) return Fail(L"http recv timeout");
        // 전체 타임아웃
        nfc::HttpRequest tot = Req(port, "/slow", mode);
        tot.totalTimeoutMs = 300;
        Http(k, tot);
        if (!WaitHttp(k, 5, 10000) || k->last.error != nfc::HTTP_ERR_TIMEOUT) return Fail(L"http total timeout");
        // 4) 리다이렉트 횟수
        Http(k, Req(port, "/redir/3", mode));
        if (!WaitHttp(k, 6, 10000) || !k->last.Ok() || k->last.BodyText() != "done" || k->last.redirects != 3)
            return Fail(L"http redirect ok");
        Http(k, Req(port, "/redir/10", mode));
        if (!WaitHttp(k, 7, 10000) || k->last.error != nfc::HTTP_ERR_REDIRECT_LIMIT) return Fail(L"http redirect limit");
        // 5) 헤더 한도
        nfc::HttpRequest hdr = Req(port, "/hdr", mode);
        hdr.maxHeaderBytes = 1024;
        Http(k, hdr);
        if (!WaitHttp(k, 8, 10000) || k->last.error != nfc::HTTP_ERR_HEADER_TOO_LARGE) return Fail(L"http header limit");
        // 6) 취소 (중복·다른 스레드)
        std::shared_ptr<nfc::HttpCall> c = Http(k, Req(port, "/slow", mode));
        if (!c) return Fail(L"http cancel send");
        std::thread t([c]() { c->Cancel(); c->Cancel(); });
        c->Cancel();
        t.join();
        if (!WaitHttp(k, 9, 5000) || k->last.error != nfc::HTTP_ERR_CANCELLED || !c->IsDone())
            return Fail(L"http cancel");
        // 7) 소유자 소멸 후 응답 도착 → 조용히 버림, 요청 정리
        std::shared_ptr<HttpSink> gone(new HttpSink());
        std::weak_ptr<HttpSink> wg = gone;
        std::shared_ptr<nfc::HttpCall> gc = Http(gone, Req(port, "/slow", mode));
        gone.reset();
        if (!PumpUI([gc]() { return gc->IsDone(); }, 8000)) return Fail(L"http owner gone not cleaned");
        if (!wg.expired()) return Fail(L"http owner kept alive");
        // 8) 잘못된 헤더 (CR/LF 주입)
        nfc::HttpRequest inj = Req(port, "/hello", mode);
        inj.headers.push_back(nfc::HttpHeader());
        inj.headers.back().name = L"X-A";
        inj.headers.back().value = L"1\r\nX-B: 2";
        Http(k, inj);
        if (!WaitHttp(k, 10, 3000) || k->last.error != nfc::HTTP_ERR_BAD_REQUEST) return Fail(L"http header injection");
    }
    // 9) 동시 요청 상한 + 큐
    { Trace(23);
        nfc::HttpClient::SetMaxConcurrent(1);
        std::shared_ptr<HttpSink> k(new HttpSink());
        for (int i = 0; i < 3; ++i) Http(k, Req(port, "/hello"));
        if (nfc::HttpClient::QueuedCount() < 1) return Fail(L"http queue");
        if (!WaitHttp(k, 3, 10000) || !k->last.Ok()) return Fail(L"http queued complete");
        nfc::HttpClient::SetMaxConcurrent(4);
    }
    srv->Close();
    return true;
}

// 종료 정리: 진행 중 요청 취소, 열린 소켓 닫기, 이후 요청 거부 (마지막에 실행)
bool TestNetShutdown() {
    std::shared_ptr<nfc::TcpListener> srv = StartHttpServer();
    std::shared_ptr<HttpSink> hk(new HttpSink());
    Http(hk, Req(srv->Port(), "/slow"));
    Http(hk, Req(srv->Port(), "/slow", nfc::Mode::Own));
    std::shared_ptr<Sink> k(new Sink());
    std::shared_ptr<nfc::Socket> s = MakeClient(nfc::Mode::Delegate, k);
    s->Connect("127.0.0.1", srv->Port());
    std::shared_ptr<nfc::Socket> own = MakeClient(nfc::Mode::Own, k);
    own->Connect("127.0.0.1", srv->Port());
    if (!PumpUI([k]() { return k->Count(nfc::NET_CONNECTED) == 2; }, 5000)) return Fail(L"shutdown setup");
    nfc::HttpClient::Shutdown();
    if (nfc::HttpClient::ActiveCount() != 0) return Fail(L"shutdown http active");
    if (!WaitHttp(hk, 2, 3000) || hk->last.error != nfc::HTTP_ERR_SHUTDOWN) return Fail(L"shutdown http result");
    if (Http(hk, Req(srv->Port(), "/hello"))) return Fail(L"shutdown http accepted");
    nfc::NetworkManager::Shutdown();
    PumpPending();
    if (!s->IsClosed() || !own->IsClosed()) return Fail(L"shutdown sockets open");
    if (nfc::Socket::Create() || nfc::TcpListener::Create(0) || s->Connect("127.0.0.1", 1) ||
            nfc::NetworkManager::RunAfter(1, []() {}))
        return Fail(L"shutdown requests accepted");
    if (nfc::NetworkManager::GetStats().connections != 0) return Fail(L"shutdown connections");
    return true;
}
} // namespace

// 9패치 영역/늘임 지정 기하 테스트 (그리기 없이 순수 계산)
bool SelfTest::TestNinePatchGeometry() {
    nfc::NinePatch np;
    np.SetRegion(0, 0, 100, 60);
    np.SetStretch(30, 20, 70, 40);
    int l, t, r, b;
    np.GetSlices(l, t, r, b);
    if (l != 30 || t != 20 || r != 30 || b != 20) {
        wprintf(L"selftest: slices %d,%d,%d,%d\n", l, t, r, b);
        return false;
    }
    if (np.MinWidth() != 60 || np.MinHeight() != 40) {
        wprintf(L"selftest: minsize\n");
        return false;
    }
    int x0, y0, x1, y1;
    np.GetStretch(x0, y0, x1, y1);
    if (x0 != 30 || y0 != 20 || x1 != 70 || y1 != 40) {
        wprintf(L"selftest: stretch roundtrip\n");
        return false;
    }
    np.SetStretch(-5, -5, 500, 500);
    np.GetStretch(x0, y0, x1, y1);
    if (x0 != 0 || y0 != 0 || x1 != 100 || y1 != 60) {
        wprintf(L"selftest: stretch clamp\n");
        return false;
    }
    std::unique_ptr<nfc::NinePatch> sub(
        nfc::NinePatch::Default()->Sub(0, 0, 100, 60));
    if (!sub.get() || sub->ImageWidth() != 100 || sub->ImageHeight() != 60) {
        wprintf(L"selftest: sub region\n");
        return false;
    }
    return true;
}

// 버튼은 명시 영역을 부모 스킨에서 잘라 그린다 (부모 스킨 없으면 기본 스킨)
bool SelfTest::TestButtonSkinRegion() {
    nfc::Dialog d;
    std::shared_ptr<nfc::Button> b(new nfc::Button());
    b->SetBounds(0, 0, 100, 32);
    d.AddControl(b);
    std::unique_ptr<nfc::NinePatch> red(
        nfc::NinePatch::CreateRoundRect(48, 24, 4,
            Gdiplus::Color(255, 255, 0, 0),
            Gdiplus::Color(255, 255, 0, 0), 0.0f));
    d.SetSkin(red.get());
    b->SetSkinRegion(0, 0, 0, 48, 24, 4, 4, 4, 4);
    {
        Gdiplus::Bitmap bmp(100, 32, PixelFormat32bppARGB);
        {
            Gdiplus::Graphics g(&bmp);
            b->Paint(g);
        }
        Gdiplus::Color c;
        bmp.GetPixel(50, 16, &c);
        if (c.GetR() != 255 || c.GetG() != 0 || c.GetB() != 0) {
            wprintf(L"selftest: parent skin region\n");
            return false;
        }
    }
    d.SetSkin(NULL);
    b->SetSkinRegion(0, 8, 92, 334, 32);
    {
        Gdiplus::Bitmap bmp(100, 32, PixelFormat32bppARGB);
        {
            Gdiplus::Graphics g(&bmp);
            b->Paint(g);
        }
        Gdiplus::Color c;
        bmp.GetPixel(50, 16, &c);
        if (c.GetA() != 255 || c.GetR() < 0xD0 || c.GetG() < 0xD0
                || c.GetB() < 0xD0) {
            wprintf(L"selftest: default skin region\n");
            return false;
        }
    }
    return true;
}

// 부모-자식: 다이얼로그가 죽으면 부모가 끊긴다
bool SelfTest::TestParentChild() {
    std::shared_ptr<nfc::Button> b(new nfc::Button());
    {
        nfc::Dialog d;
        d.AddControl(b);
        if (b->GetParent() != &d) {
            wprintf(L"selftest: parent not set\n");
            return false;
        }
    }
    if (b->GetParent() != NULL) {
        wprintf(L"selftest: parent not cleared\n");
        return false;
    }
    b->Invalidate();
    return true;
}

// NinePatch 값 복사
bool SelfTest::TestNinePatchCopy() {
    nfc::NinePatch a;
    a.SetRegion(0, 0, 100, 60);
    a.SetStretch(30, 20, 70, 40);
    nfc::NinePatch bobj = a;
    int sl, st, sr, sb;
    bobj.GetSlices(sl, st, sr, sb);
    if (sl != 30 || st != 20 || sr != 30 || sb != 20) {
        wprintf(L"selftest: copy ctor\n");
        return false;
    }
    nfc::NinePatch cobj;
    cobj = a;
    cobj.GetSlices(sl, st, sr, sb);
    if (sl != 30 || st != 20 || sr != 30 || sb != 20) {
        wprintf(L"selftest: copy assign\n");
        return false;
    }
    std::unique_ptr<nfc::NinePatch> cl(a.Clone());
    cl->GetSlices(sl, st, sr, sb);
    if (sl != 30 || st != 20 || sr != 30 || sb != 20) {
        wprintf(L"selftest: clone\n");
        return false;
    }
    return true;
}

// 부모가 자식에게 알려 주는 원본 영역 (9패치 역매핑, 순수 계산)
bool SelfTest::TestMapToSource() {
    std::unique_ptr<nfc::NinePatch> np(nfc::NinePatch::CreateRoundRect(100, 60, 4,
        Gdiplus::Color(255, 0, 0, 255), Gdiplus::Color(255, 0, 0, 255), 0.0f));
    np->SetSlices(30, 20, 30, 20);
    nfc::SkinRegion s;
    if (!np->MapToSource(0, 0, 200, 100, 200, 100, s) ||
            s.x != 0 || s.y != 0 || s.w != 100 || s.h != 60) {
        wprintf(L"selftest: map full\n");
        return false;
    }
    if (!np->MapToSource(0, 0, 30, 20, 200, 100, s) ||
            s.x != 0 || s.y != 0 || s.w != 30 || s.h != 20) {
        wprintf(L"selftest: map corner\n");
        return false;
    }
    if (!np->MapToSource(30, 20, 140, 60, 200, 100, s) ||
            s.x != 30 || s.y != 20 || s.w != 40 || s.h != 20) {
        wprintf(L"selftest: map center\n");
        return false;
    }
    if (!np->MapToSource(100, 50, 70, 50, 200, 100, s) ||
            s.x != 50 || s.y != 30 || s.w != 20 || s.h != 30) {
        wprintf(L"selftest: map mid %d,%d,%d,%d\n", s.x, s.y, s.w, s.h);
        return false;
    }
    return true;
}

// 스킨 없는 라벨은 부모가 알려 준 자기 자리 스킨을 그린다 / 자기 스킨이면 그것
bool SelfTest::TestLabelSkin() {
    nfc::Dialog d;
    std::unique_ptr<nfc::NinePatch> green(nfc::NinePatch::CreateRoundRect(60, 60, 4,
        Gdiplus::Color(255, 0, 255, 0), Gdiplus::Color(255, 0, 255, 0), 0.0f));
    std::unique_ptr<nfc::NinePatch> blue(nfc::NinePatch::CreateRoundRect(20, 20, 4,
        Gdiplus::Color(255, 0, 0, 255), Gdiplus::Color(255, 0, 0, 255), 0.0f));
    d.SetSkin(green.get());
    std::shared_ptr<nfc::Label> lab(new nfc::Label());
    lab->SetBounds(10, 10, 40, 40);
    d.AddControl(lab);
    Gdiplus::Bitmap bmp(60, 60, PixelFormat32bppPARGB);
    Gdiplus::Color c;
    {
        Gdiplus::Graphics g(&bmp);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        lab->Paint(g);
    }
    bmp.GetPixel(30, 30, &c);
    if (c.GetA() != 255 || c.GetG() != 255 || c.GetR() != 0) {
        wprintf(L"selftest: label parent region\n");
        return false;
    }
    lab->SetSkin(blue.get());
    {
        Gdiplus::Graphics g(&bmp);
        g.Clear(Gdiplus::Color(0, 0, 0, 0));
        lab->Paint(g);
    }
    bmp.GetPixel(30, 30, &c);
    if (c.GetB() != 255 || c.GetG() != 0) {
        wprintf(L"selftest: label own skin\n");
        return false;
    }
    d.RemoveControl(lab.get());
    return true;
}

// 기본 스킨 PNG는 PARGB로 올라온다 (UpdateLayeredWindow용 premultiplied)
bool SelfTest::TestDefaultSkin() {
    nfc::NinePatch* def = nfc::NinePatch::Default();
    if (!def->IsValid()) {
        wprintf(L"selftest: default skin invalid\n");
        return false;
    }
    return true;
}

// 버튼 영역은 스킨의 한 패널 안(경계선 앞)에 있어야 글자 뒤 색 조각이 없다
bool SelfTest::TestButtonRegion() {
    nfc::SkinRegion br = nfc::NinePatch::ButtonRegion();
    if (br.x < 0 || br.x + br.w > 114 || br.w - br.left - br.right < 1 || br.w - br.left - br.right > 4) {
        wprintf(L"selftest: button region\n");
        return false;
    }
    return true;
}

// ===================== IDD 리소스 =====================
namespace {
// 인메모리 DLGTEMPLATE(EX) 만들기 (리틀 엔디언, 항목 DWORD 정렬)
struct TemplateBuilder {
    std::vector<unsigned char> b;
    void W(unsigned short v) {
        b.push_back((unsigned char)(v & 0xFF));
        b.push_back((unsigned char)((v >> 8) & 0xFF));
    }
    void D(unsigned long v) {
        W((unsigned short)(v & 0xFFFF));
        W((unsigned short)((v >> 16) & 0xFFFF));
    }
    void S(const wchar_t* s) {
        while (*s) W((unsigned short)*s++);
        W(0);
    }
    void Ord(unsigned short o) { W(0xFFFF); W(o); }
    void Align() { while (b.size() % 4) b.push_back(0); }
    void HeaderEx(unsigned long style, unsigned short count, short cx, short cy,
            const wchar_t* title) {
        W(1); W(0xFFFF); D(0); D(0); D(style);
        W(count); W(0); W(0); W((unsigned short)cx); W((unsigned short)cy);
        W(0); W(0); S(title);
        if (style & DS_SETFONT) { W(9); W(400); b.push_back(0); b.push_back(1); S(L"Malgun Gothic"); }
    }
    // cls가 NULL이면 atom
    void ItemEx(unsigned long style, short x, short y, short cx, short cy, unsigned long id,
            const wchar_t* cls, unsigned short atom, const wchar_t* text) {
        Align();
        D(0); D(0); D(style);
        W((unsigned short)x); W((unsigned short)y); W((unsigned short)cx); W((unsigned short)cy);
        D(id);
        if (cls) S(cls); else Ord(atom);
        S(text);
        W(0); // extra
    }
};

std::vector<unsigned char> BuildSampleEx() {
    const unsigned long V = WS_CHILD | WS_VISIBLE;
    TemplateBuilder t;
    t.HeaderEx(DS_SETFONT | WS_POPUP, 6, 200, 100, L"Sample");
    t.ItemEx(V | WS_TABSTOP, 10, 20, 50, 14, 1001, L"NFC_Button", 0, L"OK");
    t.ItemEx(V | SS_CENTER, 4, 4, 100, 10, 1002, L"nfc_label", 0, L"Hello"); // 대소문자 무시
    t.ItemEx(V | WS_DISABLED | BS_PUSHBUTTON, 70, 20, 50, 14, 1003, NULL, 0x0080, L"Std");
    t.ItemEx(V | SS_LEFT, 4, 40, 60, 8, 0xFFFFFFFF, NULL, 0x0082, L"Static");
    t.ItemEx(V, 4, 60, 60, 8, 1005, L"UnknownCls", 0, L"?");
    t.ItemEx(V | BS_AUTOCHECKBOX, 4, 80, 60, 8, 1006, NULL, 0x0080, L"Check"); // 푸시버튼 아님
    return t.b;
}

class Gauge : public nfc::Label {};
} // namespace

bool SelfTest::TestDialogTemplate() {
    std::vector<unsigned char> data = BuildSampleEx();
    nfc::DialogTemplate t;
    std::wstring err;
    if (!nfc::DialogTemplate::Parse(&data[0], data.size(), t, &err)) return Fail(L"tmpl parse");
    if (!t.extended || t.title != L"Sample" || t.cx != 200 || t.cy != 100 ||
        !t.hasFont || t.fontPointSize != 9 || t.fontFace != L"Malgun Gothic")
        return Fail(L"tmpl header");
    if (t.items.size() != 6) return Fail(L"tmpl item count");
    if (t.items[0].className != L"NFC_Button" || t.items[0].classAtom != 0 ||
        t.items[0].id != 1001 || t.items[0].x != 10 || t.items[0].y != 20 ||
        t.items[0].cx != 50 || t.items[0].cy != 14 || t.items[0].text != L"OK")
        return Fail(L"tmpl item0");
    if (t.items[2].classAtom != 0x0080 || t.items[2].className != L"BUTTON") return Fail(L"tmpl atom button");
    if (t.items[3].classAtom != 0x0082 || t.items[3].className != L"STATIC" || t.items[3].id != -1)
        return Fail(L"tmpl atom static");

    // 기본 단위 8x16 → x*2, y*2
    nfc::Dialog d;
    if (!d.LoadFromTemplate(t, 8, 16)) return Fail(L"tmpl load");
    if (d.GetTemplateControlCount() != 4 || d.GetSkippedItemCount() != 2) return Fail(L"tmpl mapping count");
    if (d.GetTemplateWidth() != 400 || d.GetTemplateHeight() != 200 || d.GetTemplateTitle() != L"Sample")
        return Fail(L"tmpl size");
    std::shared_ptr<nfc::Button> ok;
    if (!d.Bind(1001, ok)) return Fail(L"bind button");
    if (ok->GetX() != 20 || ok->GetY() != 40 || ok->GetWidth() != 100 || ok->GetHeight() != 28 ||
        ok->GetText() != L"OK" || !ok->IsVisible() || !ok->IsEnabled() || ok->GetParent() != &d)
        return Fail(L"bind button geometry");
    if (!d.Get<nfc::Label>(1002)) return Fail(L"bind label");
    if (d.Get<nfc::Label>(1001)) return Fail(L"type mismatch not null");
    if (d.Get<nfc::Button>(9999)) return Fail(L"missing id not null");
    if (d.Get<nfc::Button>(-1)) return Fail(L"IDC_STATIC lookup");
    std::shared_ptr<nfc::Button> std1 = d.Get<nfc::Button>(1003);
    if (!std1 || std1->IsEnabled()) return Fail(L"std button / WS_DISABLED");
    if (d.Get<nfc::Button>(1005) || d.Get<nfc::Button>(1006)) return Fail(L"skipped item created");
    // 클릭 핸들러는 컨트롤에 직접 건다
    int clicks = 0;
    ok->Click = [&clicks]() { ++clicks; };
    ok->OnMouseDown(21, 41);
    ok->OnMouseUp(21, 41);
    if (clicks != 1) return Fail(L"bound click");

    // 표준 클래스 매핑 끄기 → 다시 읽으면 템플릿 컨트롤만 교체
    nfc::ControlRegistry::SetMapStandardClasses(false);
    bool okLoad = d.LoadFromTemplate(t, 8, 16);
    nfc::ControlRegistry::SetMapStandardClasses(true);
    if (!okLoad || d.GetTemplateControlCount() != 2 || d.GetSkippedItemCount() != 4)
        return Fail(L"std mapping off");
    if (d.Get<nfc::Button>(1003)) return Fail(L"std button still mapped");
    if (ok->GetParent() != NULL) return Fail(L"old control not removed");

    // 사용자 컨트롤 등록
    if (!nfc::RegisterControlClass(L"UnknownCls",
            [](const nfc::DialogItem&) { return std::shared_ptr<nfc::Control>(new Gauge()); }))
        return Fail(L"register");
    if (nfc::RegisterControlClass(NULL, nfc::ControlFactory())) return Fail(L"register null");
    okLoad = d.LoadFromTemplate(t, 8, 16);
    nfc::ControlRegistry::Unregister(L"unknowncls");
    if (!okLoad || d.GetTemplateControlCount() != 5 || !d.Get<Gauge>(1005)) return Fail(L"custom class");
    if (nfc::ControlRegistry::IsRegistered(L"UnknownCls")) return Fail(L"unregister");
    if (!nfc::ControlRegistry::IsRegistered(L"NFC_BUTTON")) return Fail(L"builtin");

    // DPI: 192에서 기본 단위가 96보다 커야 한다
    int bx96 = 0, by96 = 0, bx192 = 0, by192 = 0;
    t.ComputeBaseUnits(96, bx96, by96);
    t.ComputeBaseUnits(192, bx192, by192);
    if (bx96 <= 0 || by96 <= 0 || bx192 <= bx96 || by192 <= by96) return Fail(L"base units dpi");
    return true;
}

bool SelfTest::TestDialogTemplateSafety() {
    std::vector<unsigned char> data = BuildSampleEx();
    // 모든 잘린 길이: 실패해야 하고 크래시하면 안 된다 (힙 경계 검사를 위해 정확한 크기로 복사)
    for (size_t n = 0; n < data.size(); ++n) {
        std::vector<unsigned char> cut(data.begin(), data.begin() + n);
        nfc::DialogTemplate t;
        if (nfc::DialogTemplate::Parse(cut.empty() ? NULL : &cut[0], n, t)) return Fail(L"truncated accepted");
        nfc::Dialog d;
        if (d.LoadFromTemplate(cut.empty() ? NULL : &cut[0], n, 96)) return Fail(L"truncated loaded");
    }
    // 항목 수가 터무니없이 큼
    {
        TemplateBuilder b;
        b.HeaderEx(WS_POPUP, 0xFFFF, 10, 10, L"x");
        nfc::DialogTemplate t;
        if (nfc::DialogTemplate::Parse(&b.b[0], b.b.size(), t)) return Fail(L"huge count accepted");
    }
    // 끝(0) 없는 문자열
    {
        TemplateBuilder b;
        b.W(1); b.W(0xFFFF); b.D(0); b.D(0); b.D(WS_POPUP);
        b.W(0); b.W(0); b.W(0); b.W(10); b.W(10); b.W(0); b.W(0);
        b.W(L'A'); b.W(L'B');
        nfc::DialogTemplate t;
        if (nfc::DialogTemplate::Parse(&b.b[0], b.b.size(), t)) return Fail(L"unterminated accepted");
    }
    // 쓰레기 데이터: 결과는 상관없고 크래시만 안 하면 된다
    unsigned long seed = 12345;
    for (int k = 0; k < 2000; ++k) {
        std::vector<unsigned char> junk = data;
        for (int m = 0; m < 4; ++m) {
            seed = seed * 1103515245UL + 12345UL;
            junk[(seed >> 8) % junk.size()] = (unsigned char)(seed >> 16);
        }
        nfc::Dialog d;
        d.LoadFromTemplate(&junk[0], junk.size(), 96);
    }
    // 고전 DLGTEMPLATE (EX 아님): WORD ID 0xFFFF = IDC_STATIC
    {
        TemplateBuilder b;
        b.D(DS_SETFONT | WS_POPUP); b.D(0); b.W(2); b.W(0); b.W(0); b.W(100); b.W(50);
        b.W(0); b.W(0); b.S(L"Old"); b.W(8); b.S(L"MS Shell Dlg");
        b.Align(); b.D(WS_CHILD | WS_VISIBLE); b.D(0); b.W(5); b.W(6); b.W(30); b.W(10); b.W(0xFFFF);
        b.Ord(0x0082); b.S(L"txt"); b.W(0);
        b.Align(); b.D(WS_CHILD | WS_VISIBLE); b.D(0); b.W(5); b.W(20); b.W(30); b.W(12); b.W(7);
        b.S(L"NFC_Button"); b.S(L"Go"); b.W(0);
        nfc::DialogTemplate t;
        if (!nfc::DialogTemplate::Parse(&b.b[0], b.b.size(), t)) return Fail(L"classic parse");
        if (t.extended || t.items.size() != 2 || t.items[0].id != -1 ||
            t.items[0].className != L"STATIC" || t.items[1].id != 7 ||
            t.items[1].className != L"NFC_Button" || t.items[1].y != 20 || t.fontFace != L"MS Shell Dlg")
            return Fail(L"classic fields");
        nfc::Dialog d;
        if (!d.LoadFromTemplate(t, 4, 8) || !d.Get<nfc::Button>(7) || d.GetTemplateControlCount() != 2)
            return Fail(L"classic load");
    }
    return true;
}

// 데모 .rc의 IDD_DEMO_FORM (리소스 편집기에서 여는 그 대화상자)
bool SelfTest::TestDemoFormResource() {
    nfc::DialogTemplate t;
    std::wstring err;
    if (!nfc::DialogTemplate::LoadFromResource(GetModuleHandle(NULL), IDD_DEMO_FORM, t, &err))
        return Fail(L"demo form resource");
    if (t.items.size() != 3) return Fail(L"demo form item count");
    int buttons = 0, labels = 0;
    for (size_t i = 0; i < t.items.size(); ++i) {
        if (t.items[i].className == L"NFC_Button") ++buttons;
        if (t.items[i].className == L"NFC_Label") ++labels;
    }
    if (buttons != 2 || labels != 1) return Fail(L"demo form classes");
    nfc::Dialog d;
    if (!d.LoadFromResource(GetModuleHandle(NULL), IDD_DEMO_FORM)) return Fail(L"demo form load");
    std::shared_ptr<nfc::Button> hello, close;
    std::shared_ptr<nfc::Label> status;
    if (!d.Bind(IDC_FORM_HELLO, hello) || !d.Bind(IDC_FORM_CLOSE, close) ||
        !d.Bind(IDC_FORM_STATUS, status))
        return Fail(L"demo form bind");
    if (hello->GetWidth() <= 0 || hello->GetX() >= close->GetX() ||
        d.GetTemplateWidth() <= 0 || d.GetSkippedItemCount() != 0)
        return Fail(L"demo form layout");
    if (d.LoadFromResource(GetModuleHandle(NULL), 0x7FFF)) return Fail(L"missing IDD loaded");
    return true;
}

int SelfTest::Run() {
    // 회귀 테스트: GDI+ 캐시를 건드린 뒤 Shutdown → 깨끗이 종료해야 함.
    // 예전 버그는 종료 시 정적 소멸자가 끝난 GDI+를 건드려 AV.
    if (!nfc::App::Init(GetModuleHandle(NULL))) {
        wprintf(L"selftest: init failed\n");
        return 1;
    }
    nfc::NinePatch::Default();
    nfc::Label::DefaultFont();
    if (!TestNinePatchGeometry()) return 1;
    if (!TestButtonSkinRegion()) return 1;
    if (!TestParentChild()) return 1;
    if (!TestNinePatchCopy()) return 1;
    if (!TestMapToSource()) return 1;
    if (!TestLabelSkin()) return 1;
    if (!TestDefaultSkin()) return 1;
    if (!TestButtonRegion()) return 1;
    if (!TestDialogTemplate()) return 1;
    if (!TestDialogTemplateSafety()) return 1;
    if (!TestDemoFormResource()) return 1;
    if (!TestTasks()) return 1;
    if (!TestCString()) return 1;
    if (!TestChannel()) return 1;
    if (!TestSockets()) { nfc::App::Shutdown(); return 1; }
    if (!TestHttp()) { nfc::App::Shutdown(); return 1; }
    if (!TestNetShutdown()) { nfc::App::Shutdown(); return 1; }
    nfc::App::Shutdown();
    wprintf(L"selftest ok\n");
    return 0;
}
#else
// GUI 빌드에는 셀프테스트가 없다.
int SelfTest::Run() { return 1; }
#endif
