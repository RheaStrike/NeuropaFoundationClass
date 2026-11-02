#pragma once
#include "App.h"
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

// framework.h를 먼저 포함하고 쓸 것

namespace nfc {

// 구독 토큰 (RAII). 소멸·Cancel·다른 토큰 대입 시 구독이 풀린다. 이동만 가능.
// UI 스레드에서만 만들고/풀 것. 채널이 먼저 사라져도 안전하다 (채널은 weak로만 본다).
class Subscription {
public:
    Subscription() {}
    explicit Subscription(std::function<void()> cancel) : m_cancel(cancel) {}
    ~Subscription() { Cancel(); }
    Subscription(Subscription&& o) : m_cancel(std::move(o.m_cancel)) { o.m_cancel = nullptr; }
    Subscription& operator=(Subscription&& o) {
        if (this != &o) {
            Cancel();
            m_cancel = std::move(o.m_cancel);
            o.m_cancel = nullptr;
        }
        return *this;
    }
    Subscription(const Subscription&) = delete;
    Subscription& operator=(const Subscription&) = delete;

    void Cancel() {
        if (m_cancel) {
            std::function<void()> c = std::move(m_cancel);
            m_cancel = nullptr;
            c();
        }
    }
    bool IsActive() const { return static_cast<bool>(m_cancel); }

private:
    std::function<void()> m_cancel;
};

// 스레드 안전 메시지 채널. "받는 쪽은 부모/앱, 자식은 구독만" 구조의 바탕.
// * 생산자는 어느 스레드에서든 Push. 큐에 쌓인다.
// * 구독자가 있으면 UI 스레드에서(App::PostToUI) 넣은 순서대로 전달한다.
//   구독자가 없으면(자식 창이 닫힘) 계속 쌓아 두고, 다음 구독 때 쌓인 것을 순서대로 전달(Drain).
// * 메시지는 전달 시점의 모든 구독자에게 한 번 전달된 뒤 큐에서 빠진다.
// * 한도: maxCount(0 = 무제한) 초과 시 DropOldest(오래된 것 버림) 또는 Reject(새 것 거부).
// * 반드시 Create()로 만들어 shared_ptr로 소유할 것 (전달 작업은 채널을 weak로 잡는다).
// * Subscribe/Cancel/전달은 UI 스레드 전용 (AssertUiThread).
// * 핸들러 안에서 Cancel/Subscribe/Push 해도 안전(재진입). 핸들러가 모달 루프를 돌려
//   전달이 중첩되면 바깥 전달이 끝난 뒤 이어서 전달한다 (순서 유지).
template <typename T>
class Channel : public CObject {
public:
    enum OverflowPolicy { DropOldest, Reject };

    static std::shared_ptr<Channel> Create(size_t maxCount = 0,
            OverflowPolicy policy = DropOldest) {
        return std::shared_ptr<Channel>(new Channel(maxCount, policy));
    }

    void SetLimit(size_t maxCount, OverflowPolicy policy) {
        std::lock_guard<std::mutex> l(m_mutex);
        m_max = maxCount;
        m_policy = policy;
        while (m_max > 0 && m_queue.size() > m_max) {
            m_queue.pop_front();
            ++m_dropped;
        }
    }

    // 임의 스레드. Reject 정책으로 거부되면 false.
    bool Push(const T& v) {
        bool schedule = false;
        {
            std::lock_guard<std::mutex> l(m_mutex);
            if (m_max > 0 && m_queue.size() >= m_max) {
                if (m_policy == Reject) {
                    ++m_rejected;
                    return false;
                }
                m_queue.pop_front();
                ++m_dropped;
            }
            m_queue.push_back(v);
            schedule = NeedScheduleLocked();
        }
        if (schedule) ScheduleDeliver();
        return true;
    }

    size_t Pending() const { std::lock_guard<std::mutex> l(m_mutex); return m_queue.size(); }
    size_t Dropped() const { std::lock_guard<std::mutex> l(m_mutex); return m_dropped; }
    size_t Rejected() const { std::lock_guard<std::mutex> l(m_mutex); return m_rejected; }
    int SubscriberCount() const { return m_active.load(); }
    // 쌓인 것을 버린다 (임의 스레드)
    void Clear() { std::lock_guard<std::mutex> l(m_mutex); m_queue.clear(); }

    // UI 스레드. 쌓인 것이 있으면 곧바로(PostToUI로) 순서대로 전달된다.
    Subscription Subscribe(std::function<void(const T&)> handler) {
        App::AssertUiThread();
        std::shared_ptr<Slot> slot(new Slot());
        slot->fn = [handler](const T& v) { handler(v); return true; };
        return AddSlot(slot);
    }

    // 대상 객체를 weak로 잡는다. 전달 시점에 대상이 죽었으면 건너뛰고 구독을 끝낸다.
    template <typename O>
    Subscription Subscribe(const std::shared_ptr<O>& target,
            std::function<void(O&, const T&)> handler) {
        App::AssertUiThread();
        std::weak_ptr<O> weak = target;
        std::shared_ptr<Slot> slot(new Slot());
        slot->fn = [weak, handler](const T& v) {
            std::shared_ptr<O> t = weak.lock();
            if (!t) return false;
            handler(*t, v);
            return true;
        };
        return AddSlot(slot);
    }

private:
    struct Slot {
        Slot() : active(true) {}
        bool active; // UI 스레드에서만 읽고 쓴다
        std::function<bool(const T&)> fn; // false = 대상 소멸 → 구독 종료
    };

    Channel(size_t maxCount, OverflowPolicy policy)
        : m_max(maxCount), m_policy(policy), m_dropped(0), m_rejected(0),
          m_active(0), m_scheduled(false), m_delivering(false) {}

    // m_mutex 잡은 상태. 구독자가 있고, 큐가 있고, 아직 예약 안 됐으면 예약.
    bool NeedScheduleLocked() {
        if (m_active.load() <= 0 || m_queue.empty() || m_scheduled) return false;
        m_scheduled = true;
        return true;
    }

    void ScheduleDeliver() {
        std::weak_ptr<Channel> weak =
            std::static_pointer_cast<Channel>(WeakSelf().lock());
        if (weak.expired()) { // Create로 만들지 않음
            std::lock_guard<std::mutex> l(m_mutex);
            m_scheduled = false;
            return;
        }
        App::PostToUI([weak]() {
            std::shared_ptr<Channel> ch = weak.lock();
            if (ch) ch->Deliver(); // 채널이 먼저 사라졌으면 아무것도 안 한다
        });
    }

    Subscription AddSlot(const std::shared_ptr<Slot>& slot) {
        m_slots.push_back(slot);
        ++m_active;
        bool schedule;
        {
            std::lock_guard<std::mutex> l(m_mutex);
            schedule = NeedScheduleLocked();
        }
        if (schedule) ScheduleDeliver();
        std::weak_ptr<Channel> weakCh =
            std::static_pointer_cast<Channel>(WeakSelf().lock());
        std::weak_ptr<Slot> weakSlot = slot;
        return Subscription([weakCh, weakSlot]() {
            App::AssertUiThread();
            std::shared_ptr<Slot> s = weakSlot.lock();
            std::shared_ptr<Channel> ch = weakCh.lock();
            if (ch && s) ch->RemoveSlot(s);
        });
    }

    void RemoveSlot(const std::shared_ptr<Slot>& s) {
        if (!s->active) return;
        s->active = false; // 이미 예약된 전달도 실행 시점에 건너뛴다
        --m_active;
        for (size_t i = 0; i < m_slots.size(); ++i) {
            if (m_slots[i] == s) {
                m_slots.erase(m_slots.begin() + i);
                break;
            }
        }
    }

    // UI 스레드. 한 번에 하나씩 꺼내 전달 시점의 구독자 전원에게 준다.
    void Deliver() {
        App::AssertUiThread();
        {
            std::lock_guard<std::mutex> l(m_mutex);
            m_scheduled = false;
        }
        if (m_delivering) return; // 중첩 전달 방지: 바깥 루프가 이어서 처리
        m_delivering = true;
        std::shared_ptr<Channel> keep =
            std::static_pointer_cast<Channel>(WeakSelf().lock()); // 핸들러가 채널을 놓아도 유지
        for (;;) {
            T v;
            {
                std::lock_guard<std::mutex> l(m_mutex);
                if (m_active.load() <= 0 || m_queue.empty()) break;
                v = m_queue.front();
                m_queue.pop_front();
            }
            std::vector<std::shared_ptr<Slot> > snap(m_slots); // 전달 중 목록 변경 대비
            for (size_t i = 0; i < snap.size(); ++i) {
                std::shared_ptr<Slot> s = snap[i];
                if (!s->active) continue; // 전달 도중 해제됨
                if (!s->fn(v)) RemoveSlot(s); // 대상 소멸
            }
        }
        m_delivering = false;
    }

    mutable std::mutex m_mutex;          // 큐·카운터·예약 플래그 보호 (Push는 임의 스레드)
    std::deque<T> m_queue;
    size_t m_max;
    OverflowPolicy m_policy;
    size_t m_dropped;
    size_t m_rejected;
    std::atomic<int> m_active;           // 활성 구독자 수 (Push가 읽는다)
    bool m_scheduled;                    // PostToUI 예약됨
    bool m_delivering;                   // UI 스레드 전용
    std::vector<std::shared_ptr<Slot> > m_slots; // UI 스레드 전용
};

} // namespace nfc
