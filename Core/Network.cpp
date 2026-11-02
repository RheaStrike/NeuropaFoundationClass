// asio를 쓰는 유일한 번역 단위. winsock2.h가 windows.h보다 먼저 와야 하므로
// 이 파일만 예외로 targetver.h → winsock2.h → framework.h 순서로 포함한다.
#include "targetver.h"
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include "framework.h"

#ifndef NFC_USE_ASIO
#error "Network.cpp requires NFC_USE_ASIO (Core/ThirdParty/asio)"
#endif

#ifdef _MSC_VER
#pragma warning(push)
#pragma warning(disable: 4996 4267 4244 4702) // asio 내부 경고만 억제
#endif
#define ASIO_STANDALONE
#define ASIO_NO_DEPRECATED
#include <asio.hpp>
#ifdef _MSC_VER
#pragma warning(pop)
#endif
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "mswsock.lib")

#include "NetworkManager.h"
#include "Socket.h"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <thread>

namespace nfc {

using asio::ip::tcp;
typedef asio::io_context::executor_type IoExec;
typedef asio::executor_work_guard<IoExec> WorkGuard;

namespace {

// Shutdown 때 닫을 수 있는 것 (소켓·리스너·타이머)
struct Closable {
    virtual ~Closable() {}
    virtual void CloseAsync() = 0;
};

struct Mgr {
    Mgr() : running(false), threadCount(0), pending(0), exceptions(0),
        connections(0), totalConnects(0) {}
    std::mutex mutex;
    std::unique_ptr<asio::io_context> io; // 한 번 만들면 프로세스 끝까지 유지 (소켓이 참조)
    std::unique_ptr<WorkGuard> work;
    std::vector<std::thread> threads;
    std::vector<std::weak_ptr<Closable> > registry;
    bool running;
    unsigned threadCount;
    std::atomic<long> pending;
    std::atomic<long> exceptions;
    std::atomic<long> connections;
    std::atomic<long> totalConnects;
    std::mutex idleMutex;
    std::condition_variable idleCv;
};

Mgr& M() {
    static Mgr m;
    return m;
}

void Register(const std::shared_ptr<Closable>& c) {
    Mgr& m = M();
    std::lock_guard<std::mutex> l(m.mutex);
    // 죽은 항목 정리
    for (size_t i = 0; i < m.registry.size();) {
        if (m.registry[i].expired()) m.registry.erase(m.registry.begin() + i);
        else ++i;
    }
    m.registry.push_back(c);
}

void PendingInc() { ++M().pending; }
void PendingDec() {
    if (--M().pending <= 0) {
        std::lock_guard<std::mutex> l(M().idleMutex);
        M().idleCv.notify_all();
    }
}

// Mode::Own용 자기 io_context + 전용 스레드
struct OwnExec {
    OwnExec() : work(new WorkGuard(io.get_executor())) {
        thread = std::thread([this]() { RunIo(); });
    }
    void RunIo() {
        for (;;) {
            try {
                io.run();
                return;
            } catch (...) {
                ++M().exceptions; // 핸들러 밖으로 샌 예외도 스레드를 죽이지 않는다
            }
        }
    }
    // 작업 보증을 풀고 남은 핸들러(닫기·취소 완료)를 끝낸 뒤 stop+join
    void Stop() {
        work.reset();
        if (thread.joinable()) {
            if (thread.get_id() == std::this_thread::get_id()) {
                io.stop();
                thread.detach(); // 자기 스레드에서 마지막 참조가 풀린 경우
            } else {
                thread.join();
            }
        }
        io.stop();
    }
    ~OwnExec() { Stop(); }
    asio::io_context io;
    std::unique_ptr<WorkGuard> work;
    std::thread thread;
};

void RunPoolThread() {
    for (;;) {
        try {
            M().io->run();
            return;
        } catch (...) {
            ++M().exceptions;
        }
    }
}

std::string EcText(const asio::error_code& ec) {
    std::string s = ec.message();
    for (size_t i = 0; i < s.size(); ++i) if ((unsigned char)s[i] > 127) s[i] = '?';
    return s;
}

} // namespace

// ---------------------------------------------------------------------------
// 소켓 구현. 모든 멤버는 strand 안에서만 만진다 (atomic 제외).
struct Socket::Impl : public Closable, public std::enable_shared_from_this<Socket::Impl> {
    // own을 가장 먼저 선언: 가장 나중에 파괴되어 asio 객체보다 io가 오래 산다
    std::shared_ptr<OwnExec> own;
    asio::strand<IoExec> strand;
    tcp::socket sock;
    tcp::resolver resolver;
    std::weak_ptr<Socket> owner;
    std::atomic<bool> closed;
    std::atomic<size_t> queued;
    std::atomic<size_t> maxQueued;
    bool connected;
    bool closeDone;
    bool writing;
    bool closeWhenSent;
    std::deque<Bytes> sendQ;
    std::array<unsigned char, 16384> rbuf;
    std::function<void(Socket&, const Bytes&)> ioRecv;

    Impl(asio::io_context& io, const std::shared_ptr<OwnExec>& ownExec)
        : own(ownExec), strand(asio::make_strand(io.get_executor())), sock(strand),
          resolver(strand), closed(false), queued(0), maxQueued(1024 * 1024),
          connected(false), closeDone(false), writing(false), closeWhenSent(false) {}

    // 핸들러 공통: 대기 수 감소 + 예외 격리 (이 소켓만 닫는다)
    void Guarded(const std::function<void()>& f) {
        PendingDec();
        try {
            f();
        } catch (const std::exception& e) {
            ++M().exceptions;
            Fail(-1, std::string("handler exception: ") + e.what());
        } catch (...) {
            ++M().exceptions;
            Fail(-1, "handler exception");
        }
    }

    void Emit(NetEventType t, int code, const std::string& msg) {
        std::shared_ptr<Socket> s = owner.lock();
        if (!s) return;
        NetEvent ev;
        ev.type = t;
        ev.code = code;
        ev.message = msg;
        s->PostEvent(ev);
    }

    void Connect(const std::string& host, unsigned short port) {
        std::shared_ptr<Impl> self = shared_from_this();
        PendingInc();
        resolver.async_resolve(host, std::to_string(port),
            [self](const asio::error_code& ec, tcp::resolver::results_type results) {
                self->Guarded([&]() { self->OnResolve(ec, results); });
            });
    }

    void OnResolve(const asio::error_code& ec, const tcp::resolver::results_type& results) {
        if (closeDone) return;
        if (ec) {
            Fail(ec.value(), "resolve: " + EcText(ec));
            return;
        }
        std::shared_ptr<Impl> self = shared_from_this();
        PendingInc();
        asio::async_connect(sock, results,
            [self](const asio::error_code& ec2, const tcp::endpoint&) {
                self->Guarded([&]() { self->OnConnect(ec2); });
            });
    }

    void OnConnect(const asio::error_code& ec) {
        if (closeDone) return;
        if (ec) {
            Fail(ec.value(), "connect: " + EcText(ec));
            return;
        }
        MarkConnected();
        Emit(NET_CONNECTED, 0, "connected");
        StartRead();
        if (!sendQ.empty() && !writing) StartWrite();
    }

    void MarkConnected() {
        connected = true;
        ++M().connections;
        ++M().totalConnects;
    }

    void StartRead() {
        if (closeDone) return;
        std::shared_ptr<Impl> self = shared_from_this();
        PendingInc();
        sock.async_read_some(asio::buffer(rbuf),
            [self](const asio::error_code& ec, std::size_t n) {
                self->Guarded([&]() { self->OnRead(ec, n); });
            });
    }

    void OnRead(const asio::error_code& ec, std::size_t n) {
        if (closeDone) return;
        if (ec) {
            if (ec == asio::error::eof || ec == asio::error::connection_reset ||
                    ec == asio::error::connection_aborted) {
                DoClose(); // 상대가 끊음 → Disconnected
            } else if (ec != asio::error::operation_aborted) {
                Fail(ec.value(), "read: " + EcText(ec));
            }
            return;
        }
        Bytes data(rbuf.begin(), rbuf.begin() + n);
        if (ioRecv) {
            std::shared_ptr<Socket> s = owner.lock();
            if (s) ioRecv(*s, data); // 예외는 Guarded가 잡는다
        }
        if (closeDone) return;
        std::shared_ptr<Socket> s = owner.lock();
        if (s) s->PostData(data);
        StartRead();
    }

    bool Send(const Bytes& data) {
        if (closed.load() || data.empty()) return !closed.load() && data.empty();
        size_t n = data.size();
        size_t before = queued.fetch_add(n);
        if (before + n > maxQueued.load()) {
            queued.fetch_sub(n);
            return false; // 전송 큐 한도
        }
        std::shared_ptr<Impl> self = shared_from_this();
        asio::post(strand, [self, data]() {
            if (self->closeDone) {
                self->queued.fetch_sub(data.size());
                return;
            }
            self->sendQ.push_back(data);
            if (self->connected && !self->writing) self->StartWrite();
        });
        return true;
    }

    void StartWrite() {
        if (sendQ.empty() || closeDone) return;
        writing = true;
        std::shared_ptr<Impl> self = shared_from_this();
        PendingInc();
        asio::async_write(sock, asio::buffer(sendQ.front()),
            [self](const asio::error_code& ec, std::size_t) {
                self->Guarded([&]() { self->OnWrite(ec); });
            });
    }

    void OnWrite(const asio::error_code& ec) {
        writing = false;
        if (!sendQ.empty()) {
            queued.fetch_sub(sendQ.front().size());
            sendQ.pop_front();
        }
        if (closeDone) return;
        if (ec) {
            if (ec != asio::error::operation_aborted) Fail(ec.value(), "write: " + EcText(ec));
            return;
        }
        if (!sendQ.empty()) StartWrite();
        else if (closeWhenSent) RequestClose();
    }

    void CloseWhenSent() {
        std::shared_ptr<Impl> self = shared_from_this();
        asio::post(strand, [self]() {
            if (self->sendQ.empty() && !self->writing) self->RequestClose();
            else self->closeWhenSent = true;
        });
    }

    // 어느 스레드에서든. 실제 닫기는 strand에서 한 번만.
    void RequestClose() {
        if (closed.exchange(true)) {
            return;
        }
        std::shared_ptr<Impl> self = shared_from_this();
        asio::post(strand, [self]() { self->DoClose(); });
    }

    virtual void CloseAsync() { RequestClose(); }

    // strand 안. 몇 번 불려도 한 번만 실행.
    void DoClose() {
        closed = true;
        if (closeDone) return;
        closeDone = true;
        asio::error_code ignored;
        resolver.cancel();
        if (sock.is_open()) {
            sock.shutdown(tcp::socket::shutdown_both, ignored);
            sock.close(ignored);
        }
        // 보내지 못한 것은 버린다. 쓰는 중인 앞 버퍼는 OnWrite가 빼므로 남겨 둔다.
        if (writing && !sendQ.empty()) {
            Bytes front;
            front.swap(sendQ.front());
            sendQ.clear();
            queued = front.size();
            sendQ.push_back(Bytes());
            sendQ.front().swap(front);
        } else {
            sendQ.clear();
            queued = 0;
        }
        if (connected) {
            connected = false;
            --M().connections;
            Emit(NET_DISCONNECTED, 0, "disconnected");
        }
    }

    // strand 안. 오류 이벤트 후 닫기.
    void Fail(int code, const std::string& msg) {
        if (closeDone) return;
        Emit(NET_ERROR, code, msg);
        DoClose();
    }
};

// ---------------------------------------------------------------------------
// NetworkManager

void NetworkManager::SetThreadCount(unsigned n) {
    std::lock_guard<std::mutex> l(M().mutex);
    M().threadCount = n;
}

bool NetworkManager::Start() {
    Mgr& m = M();
    std::lock_guard<std::mutex> l(m.mutex);
    if (m.running) return true;
    if (!m.io) m.io.reset(new asio::io_context());
    else m.io->restart();
    m.work.reset(new WorkGuard(m.io->get_executor()));
    unsigned n = m.threadCount;
    if (n == 0) {
        n = std::thread::hardware_concurrency() / 2;
        if (n < 2) n = 2;
        if (n > 4) n = 4;
    }
    for (unsigned i = 0; i < n; ++i) m.threads.push_back(std::thread(&RunPoolThread));
    m.running = true;
    return true;
}

void NetworkManager::Shutdown() {
    Mgr& m = M();
    std::vector<std::weak_ptr<Closable> > regs;
    {
        std::lock_guard<std::mutex> l(m.mutex);
        if (!m.running) return;
        m.running = false; // 이후 생성·연결·RunAfter 거부
        regs.swap(m.registry);
    }
    // 1) 등록된 소켓·리스너·타이머 닫기/취소
    for (size_t i = 0; i < regs.size(); ++i) {
        std::shared_ptr<Closable> c = regs[i].lock();
        if (c) c->CloseAsync();
    }
    // 2) 공용 풀의 대기 핸들러가 끝나기를 잠깐 기다린다 (취소 완료 통지). Own 소켓은 자기 스레드.
    m.work.reset();
    {
        std::unique_lock<std::mutex> l(m.idleMutex);
        m.idleCv.wait_for(l, std::chrono::seconds(3),
            [&m]() { return m.pending.load() <= 0; });
    }
    // 3) stop → join
    if (m.io) m.io->stop();
    std::vector<std::thread> ths;
    {
        std::lock_guard<std::mutex> l(m.mutex);
        ths.swap(m.threads);
    }
    for (size_t i = 0; i < ths.size(); ++i) if (ths[i].joinable()) ths[i].join();
}

bool NetworkManager::IsRunning() {
    std::lock_guard<std::mutex> l(M().mutex);
    return M().running;
}

NetStats NetworkManager::GetStats() {
    Mgr& m = M();
    NetStats s;
    {
        std::lock_guard<std::mutex> l(m.mutex);
        int alive = 0;
        for (size_t i = 0; i < m.registry.size(); ++i) if (!m.registry[i].expired()) ++alive;
        s.sockets = alive;
    }
    s.connections = (int)m.connections.load();
    s.pendingHandlers = m.pending.load();
    s.exceptions = m.exceptions.load();
    s.totalConnects = m.totalConnects.load();
    return s;
}

namespace {
struct TimerHolder : public Closable, public std::enable_shared_from_this<TimerHolder> {
    explicit TimerHolder(asio::io_context& io) : timer(io) {}
    virtual void CloseAsync() {
        std::shared_ptr<TimerHolder> self = shared_from_this();
        asio::post(timer.get_executor(), [self]() { self->timer.cancel(); });
    }
    asio::steady_timer timer;
};
} // namespace

bool NetworkManager::RunAfter(int ms, std::function<void()> f) {
    if (!f) return false;
    std::shared_ptr<TimerHolder> t;
    {
        std::lock_guard<std::mutex> l(M().mutex);
        if (!M().running) return false;
        t.reset(new TimerHolder(*M().io));
    }
    Register(t);
    t->timer.expires_after(std::chrono::milliseconds(ms < 0 ? 0 : ms));
    PendingInc();
    t->timer.async_wait([t, f](const asio::error_code& ec) {
        PendingDec();
        if (ec) return; // 취소됨
        try {
            f();
        } catch (...) {
            ++M().exceptions;
        }
    });
    return true;
}

// ---------------------------------------------------------------------------
// Socket

std::shared_ptr<Socket> Socket::Create(Mode mode) {
    std::shared_ptr<OwnExec> own;
    asio::io_context* io = NULL;
    {
        std::lock_guard<std::mutex> l(M().mutex);
        if (!M().running) return std::shared_ptr<Socket>();
        io = M().io.get();
    }
    if (mode == Mode::Own) {
        own.reset(new OwnExec());
        io = &own->io;
    }
    std::shared_ptr<Impl> impl(new Impl(*io, own));
    std::shared_ptr<Socket> s(new Socket(mode));
    s->m_impl = impl;
    impl->owner = s;
    Register(impl);
    return s;
}

std::shared_ptr<Socket> Socket::Wrap(const std::shared_ptr<Impl>& impl) {
    std::shared_ptr<Socket> s(new Socket(impl->own ? Mode::Own : Mode::Delegate));
    s->m_impl = impl;
    impl->owner = s;
    Register(impl);
    return s;
}

Socket::~Socket() {
    // 어느 스레드에서든 안전: 닫기만 예약. Own이면 자기 스레드를 stop+join.
    if (m_impl) {
        m_impl->RequestClose();
        std::shared_ptr<OwnExec> own = m_impl->own;
        if (own) own->Stop(); // 닫기·취소 완료 핸들러까지 돌고 끝난다
    }
}

void Socket::ClearHandlers() {
    App::AssertUiThread();
    m_onReceive = nullptr;
    m_onEvent = nullptr;
}

void Socket::SetChannels(const std::shared_ptr<Channel<Bytes> >& data,
        const std::shared_ptr<Channel<NetEvent> >& events) {
    std::lock_guard<std::mutex> l(m_chMutex);
    m_dataCh = data;
    m_eventCh = events;
}

void Socket::SetIoReceiveHandler(std::function<void(Socket&, const Bytes&)> h) {
    std::shared_ptr<Impl> impl = m_impl;
    asio::post(impl->strand, [impl, h]() { impl->ioRecv = h; });
}

bool Socket::Connect(const std::string& host, unsigned short port) {
    if (!NetworkManager::IsRunning() || m_impl->closed.load()) return false;
    std::shared_ptr<Impl> impl = m_impl;
    asio::post(impl->strand, [impl, host, port]() {
        if (!impl->closeDone) impl->Connect(host, port);
    });
    return true;
}

bool Socket::Send(const Bytes& data) {
    return m_impl->Send(data);
}

bool Socket::Send(const std::string& data) {
    return m_impl->Send(Bytes(data.begin(), data.end()));
}

void Socket::SetMaxSendQueue(size_t bytes) {
    m_impl->maxQueued = bytes;
}

size_t Socket::QueuedBytes() const {
    return m_impl->queued.load();
}

void Socket::CloseWhenSent() {
    if (!m_impl->closed.load()) m_impl->CloseWhenSent();
}

void Socket::Close() {
    m_impl->RequestClose();
}

bool Socket::IsClosed() const {
    return m_impl->closed.load();
}

void Socket::PostData(const Bytes& data) {
    {
        std::lock_guard<std::mutex> l(m_chMutex);
        if (m_dataCh) m_dataCh->Push(data);
    }
    std::weak_ptr<CObject> w = WeakSelf();
    App::PostToUI([w, data]() {
        std::shared_ptr<Socket> s = std::static_pointer_cast<Socket>(w.lock());
        if (s) s->DeliverData(data);
    });
}

void Socket::PostEvent(const NetEvent& ev) {
    {
        std::lock_guard<std::mutex> l(m_chMutex);
        if (m_eventCh) m_eventCh->Push(ev);
    }
    std::weak_ptr<CObject> w = WeakSelf();
    App::PostToUI([w, ev]() {
        std::shared_ptr<Socket> s = std::static_pointer_cast<Socket>(w.lock());
        if (s) s->DeliverEvent(ev);
    });
}

void Socket::DeliverData(const Bytes& data) {
    if (m_onReceive && !m_onReceive(data)) {
        // 소유자가 사라졌다: 조용히 버리고 닫는다
        m_onReceive = nullptr;
        m_onEvent = nullptr;
        Close();
    }
}

void Socket::DeliverEvent(const NetEvent& ev) {
    if (m_onEvent && !m_onEvent(ev)) {
        m_onReceive = nullptr;
        m_onEvent = nullptr;
        Close();
    }
}

// ---------------------------------------------------------------------------
// TcpListener

struct TcpListener::Impl : public Closable, public std::enable_shared_from_this<TcpListener::Impl> {
    explicit Impl(asio::io_context& io)
        : io(io), strand(asio::make_strand(io.get_executor())), acceptor(strand),
          closed(false), closeDone(false) {}
    asio::io_context& io;
    asio::strand<IoExec> strand;
    tcp::acceptor acceptor;
    std::atomic<bool> closed;
    bool closeDone;
    std::function<void(const std::shared_ptr<Socket>&)> onAccept;
    std::vector<std::shared_ptr<Socket> > accepted;

    void StartAccept() {
        if (closeDone) return;
        std::shared_ptr<Socket::Impl> peer(new Socket::Impl(io, std::shared_ptr<OwnExec>()));
        std::shared_ptr<Impl> self = shared_from_this();
        PendingInc();
        acceptor.async_accept(peer->sock, [self, peer](const asio::error_code& ec) {
            PendingDec();
            if (self->closeDone || ec) return;
            try {
                std::shared_ptr<Socket> s = Socket::Wrap(peer);
                // 수락 핸들러(입출력 핸들러 등록)를 먼저 부른 뒤 읽기를 시작한다
                if (self->onAccept) self->onAccept(s);
                asio::post(peer->strand, [peer]() {
                    peer->MarkConnected();
                    peer->StartRead();
                });
                for (size_t i = 0; i < self->accepted.size();) {
                    if (self->accepted[i]->IsClosed()) self->accepted.erase(self->accepted.begin() + i);
                    else ++i;
                }
                self->accepted.push_back(s);
            } catch (...) {
                ++M().exceptions;
            }
            self->StartAccept();
        });
    }

    virtual void CloseAsync() {
        if (closed.exchange(true)) return;
        std::shared_ptr<Impl> self = shared_from_this();
        asio::post(strand, [self]() {
            if (self->closeDone) return;
            self->closeDone = true;
            asio::error_code ignored;
            self->acceptor.close(ignored);
            for (size_t i = 0; i < self->accepted.size(); ++i) self->accepted[i]->Close();
            self->accepted.clear();
        });
    }
};

std::shared_ptr<TcpListener> TcpListener::Create(unsigned short port, bool loopbackOnly,
        std::function<void(const std::shared_ptr<Socket>&)> onAccept) {
    asio::io_context* io = NULL;
    {
        std::lock_guard<std::mutex> l(M().mutex);
        if (!M().running) return std::shared_ptr<TcpListener>();
        io = M().io.get();
    }
    std::shared_ptr<Impl> impl(new Impl(*io));
    asio::error_code ec;
    tcp::endpoint ep(loopbackOnly ? asio::ip::address(asio::ip::address_v4::loopback())
                                  : asio::ip::address(asio::ip::address_v4::any()), port);
    impl->acceptor.open(ep.protocol(), ec);
    if (!ec) impl->acceptor.bind(ep, ec);
    if (!ec) impl->acceptor.listen(asio::socket_base::max_listen_connections, ec);
    if (ec) return std::shared_ptr<TcpListener>();
    impl->onAccept = onAccept;
    std::shared_ptr<TcpListener> l(new TcpListener());
    l->m_port = impl->acceptor.local_endpoint(ec).port();
    l->m_impl = impl;
    Register(impl);
    asio::post(impl->strand, [impl]() { impl->StartAccept(); });
    return l;
}

TcpListener::~TcpListener() {
    if (m_impl) m_impl->CloseAsync();
}

unsigned short TcpListener::Port() const {
    return m_port;
}

void TcpListener::Close() {
    if (m_impl) m_impl->CloseAsync();
}

} // namespace nfc
