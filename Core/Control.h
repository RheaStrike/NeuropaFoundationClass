#pragma once
#include "App.h"
#include "SkinRegion.h"
#include <future>
#include <memory>
#include <string>

// framework.h를 먼저 포함하고 쓸 것 (Gdiplus 정의 필요)

namespace nfc {

class Dialog;    // 전방 선언
class NinePatch; // 전방 선언

// 부모가 자식에게 알려 주는 "네 자리의 스킨" (NeuroCD 스킨 철학).
// 부모는 자기 스킨을 9패치로 늘여 그리므로, 자식 영역에 대응하는 원본 영역(src)과
// 그릴 크기(dw, dh)를 함께 넘긴다. 자식은 그 영역을 스스로 그린다.
struct PaintContext {
    NinePatch* skin;    // 부모 스킨. NULL이면 알려 준 것 없음
    bool hasSource;     // src가 유효한가
    SkinRegion src;     // 부모 스킨 안의 원본 영역 (슬라이스 0 = 단순 늘이기)
    int dx, dy, dw, dh; // 그릴 위치·크기 (= 컨트롤 영역)
};

// 스킨 상태. 상태별 스킨/영역은 선택 사항이며, 없으면 보통 상태 + 반투명 틴트로 표시.
enum SkinState {
    SKIN_NORMAL = 0,
    SKIN_HOVER = 1,
    SKIN_PRESSED = 2,
    SKIN_DISABLED = 3,
    SKIN_STATE_COUNT = 4
};

// 윈도우리스 컨트롤. HWND 없이 Dialog 안에서 직접 그리고 입력받는다.
// (레이어드 윈도우는 자식 HWND를 제대로 품지 못하므로 처음부터 윈도우리스로 간다.)
// UI 스레드 전용. 자기 스레드도 strand도 없다. 무거운 일은 RunAsync로 워커에 보내고
// 완료는 UI 스레드에서 받는다. 부모 Dialog는 하나. Dialog가 죽으면 부모가 끊긴다.
//
// 스킨 고르는 순서 (상태 st):
//   1) 자기 상태 스킨 SetSkin(p, st) → 없으면 자기 보통 스킨 SetSkin(p)
//   2) 둘 다 없으면 부모가 알려 준 스킨
// 영역 고르는 순서:
//   1) 명시 영역 SetSkinRegion(st, ...) → 없으면 보통 상태 명시 영역
//   2) 자기 스킨이면 그 스킨 전체(9패치), 부모 스킨이면 부모가 알려 준 영역
class NFC_API Control : public CObject {
public:
    Control();
    virtual ~Control();

    void SetBounds(int x, int y, int w, int h) { m_x = x; m_y = y; m_w = w; m_h = h; }
    int GetX() const { return m_x; }
    int GetY() const { return m_y; }
    int GetWidth() const { return m_w; }
    int GetHeight() const { return m_h; }
    void SetText(const std::wstring& t) { m_text = t; }
    const std::wstring& GetText() const { return m_text; }
    // 컨트롤 ID (IDD 리소스의 IDC_ 값). 0 = 없음, -1 = IDC_STATIC. Dialog::Get/Bind가 이것으로 찾는다.
    void SetId(int id) { m_id = id; }
    int GetId() const { return m_id; }
    bool HitTest(int x, int y) const {
        return m_visible && x >= m_x && y >= m_y && x < m_x + m_w && y < m_y + m_h;
    }
    void SetVisible(bool v) { m_visible = v; }
    bool IsVisible() const { return m_visible; }
    void SetEnabled(bool e);
    bool IsEnabled() const { return m_enabled; }

    // --- 스킨 ---
    // 빌려 쓴다 (소유권 없음, 수명은 호출자). NULL이면 해제 → 부모 스킨.
    void SetSkin(NinePatch* skin, int state = SKIN_NORMAL);
    NinePatch* GetSkin(int state = SKIN_NORMAL) const;
    // 상태별 원본 영역 (쓰는 스킨 기준 좌표 + 고정 테두리)
    void SetSkinRegion(int state, int x, int y, int w, int h,
        int l = 8, int t = 8, int r = 8, int b = 8);
    void ClearSkinRegion(int state);
    // 자기 스킨도 명시 영역도 없을 때 부모가 알려 준 영역을 그릴지 (기본 true)
    void SetUseParentSkin(bool use) { m_useParentSkin = use; }
    // 호버/누름/비활성 시 틴트·테두리 표시 (기본 false, Button은 true)
    void SetStateFeedback(bool on) { m_stateFeedback = on; }
    int GetState() const; // SkinState
    bool IsHover() const { return m_hover; }
    bool IsPressed() const { return m_pressed; }

    // --- 그리기 ---
    // 부모에게 자기 자리를 물어 그린다 (부모 없으면 기본 스킨 기준).
    void Paint(Gdiplus::Graphics& g);
    // 부모가 알려 준 자리로 그린다 (Dialog가 부른다).
    void Paint(Gdiplus::Graphics& g, const PaintContext& ctx);

    // --- 입력 (Dialog가 전달) ---
    virtual void OnMouseDown(int x, int y);
    virtual void OnMouseUp(int x, int y);
    virtual void OnMouseMove(int x, int y);
    virtual void OnMouseLeave();
    virtual void OnCaptureLost(); // 누른 채 캡처를 잃음 → 누름 취소
    virtual void OnChar(wchar_t ch); // 포커스는 TODO. 지금은 마지막으로 누른 컨트롤이 받는다

    Dialog* GetParent() const { return m_parent; }
    void SetParent(Dialog* d) { m_parent = d; }
    void Invalidate(); // 부모 창 다시 그리기 예약 (합쳐진다)

    // 워커 풀에서 work()를 돌리고, 끝나면 UI 스레드에서 done(shared_future)을 부른다.
    // done 안에서 fut.get()으로 결과/예외를 받는다. 그 사이 컨트롤이 죽었으면 done은 불리지 않는다
    // (컨트롤은 weak로만 붙잡는다 → 작업이 컨트롤 수명을 늘리지 않음).
    // shared_ptr로 소유된 컨트롤만 가능. 아니면 false.
    // (객체별 직렬 큐가 필요한 워커 쪽 객체는 CObject::Post/RunAsync를 쓴다.)
    template <typename F, typename D>
    bool RunAsync(F work, D done) {
        typedef decltype(work()) R;
        std::weak_ptr<CObject> weak = WeakSelf();
        if (weak.expired() || !App::IsWorkerPoolRunning()) return false;
        std::shared_ptr<std::packaged_task<R()> > task(new std::packaged_task<R()>(work));
        std::shared_future<R> fut = task->get_future().share();
        App::RunWorker([task, fut, weak, done]() {
            (*task)();
            App::PostToUI([fut, weak, done]() {
                std::shared_ptr<CObject> alive = weak.lock();
                if (alive) done(fut);
            });
        });
        return true;
    }

protected:
    // 기본 = PaintSkin. 파생은 PaintSkin 후 글자 등을 그린다.
    virtual void OnPaint(Gdiplus::Graphics& g, const PaintContext& ctx);
    // 위 순서대로 스킨을 골라 그리고, 필요하면 상태 틴트·테두리를 덧그린다.
    void PaintSkin(Gdiplus::Graphics& g, const PaintContext& ctx);
    // NeuroCD DrawSkinButton식: 반투명 틴트 + 상태별 테두리 색
    void PaintStateOverlay(Gdiplus::Graphics& g, int state);
    // 상태에 맞춘 글자색 (비활성이면 흐리게)
    Gdiplus::Color StateTextColor(Gdiplus::Color normal) const;
    void DrawTextCentered(Gdiplus::Graphics& g, Gdiplus::Color c, int align);

    int m_x, m_y, m_w, m_h;
    bool m_visible;
    bool m_enabled;
    bool m_hover;
    bool m_pressed;
    bool m_useParentSkin;
    bool m_stateFeedback;
    std::wstring m_text;
    int m_id;
    Dialog* m_parent;
    NinePatch* m_skins[SKIN_STATE_COUNT];     // 빌린 것. 소유권 없음.
    SkinRegion m_regions[SKIN_STATE_COUNT];
    bool m_hasRegion[SKIN_STATE_COUNT];
};

} // namespace nfc
