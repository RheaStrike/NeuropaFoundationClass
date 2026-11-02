#pragma once
#include "Wnd.h"
#include "Control.h"
#include "Channel.h"
#include "DialogTemplate.h"
#include <memory>
#include <string>
#include <vector>

namespace nfc {

class NinePatch;

// WinForms식 대화상자. 윈도우리스 컨트롤을 품고 9패치 배경을 그린다.
// 그릴 때 각 컨트롤에게 "네 자리의 내 스킨 영역"(원본 영역 + 그릴 크기)을 알려 준다.
// UI 스레드 전용.
class NFC_API Dialog : public Wnd {
public:
    Dialog();
    virtual ~Dialog();

    // w/h <= 0이면 스킨 물리 이미지 크기로 창을 만든다.
    bool Create(Wnd* parent, const wchar_t* title, int w, int h);
    int DoModal(Wnd* parent, const wchar_t* title, int w, int h);
    void EndDialog(int result);

    // --- IDD 리소스 (VS 리소스 편집기에서 만든 대화상자) ---
    // 템플릿을 읽어 컨트롤을 만든다 (창은 만들지 않음 → CUI에서도 가능).
    // Custom Control 클래스 이름(NFC_Button 등)과 표준 BUTTON/STATIC은 ControlRegistry로 만든다.
    // 모르는 클래스는 건너뛰고(로그 한 줄) 나머지는 그대로 만든다. 잘못된/잘린 템플릿이면 false.
    // dpi <= 0이면 화면 DPI. 다시 불러도 이전에 템플릿으로 만든 컨트롤만 바꾼다.
    bool LoadFromResource(HINSTANCE inst, UINT idd, int dpi = 0);
    bool LoadFromTemplate(const void* data, size_t size, int dpi = 0);
    // 이미 읽은 템플릿으로 (기본 단위를 직접 주고) 만든다. 셀프테스트·고정 배치용.
    bool LoadFromTemplate(const DialogTemplate& t, int baseX, int baseY);
    // LoadFromResource + Create. 제목/크기는 템플릿 값(크기 0이면 스킨 크기).
    bool CreateFromResource(HINSTANCE inst, UINT idd, Wnd* parent = NULL);
    // LoadFromResource + DoModal
    int DoModal(Wnd* parent, HINSTANCE inst, UINT idd);
    // 템플릿에서 만든 컨트롤 수 / 건너뛴 항목 수 (마지막 Load 기준)
    size_t GetTemplateControlCount() const { return m_templateControls.size(); }
    size_t GetSkippedItemCount() const { return m_skippedItems; }
    const std::wstring& GetTemplateTitle() const { return m_templateTitle; }
    int GetTemplateWidth() const { return m_templateW; }   // 픽셀
    int GetTemplateHeight() const { return m_templateH; }  // 픽셀

    // --- ID로 컨트롤 찾기 ---
    // 없거나(-1/0 포함) 타입이 다르면 NULL + 디버그 로그. 예외는 던지지 않는다.
    std::shared_ptr<Control> FindControl(int id) const;
    template <typename T>
    std::shared_ptr<T> Get(int id) const {
        std::shared_ptr<Control> c = FindControl(id);
        if (!c) { LogBindMiss(id, false); return std::shared_ptr<T>(); }
        std::shared_ptr<T> t = std::dynamic_pointer_cast<T>(c);
        if (!t) LogBindMiss(id, true);
        return t;
    }
    // member = Get<T>(id). 찾았으면 true. 이벤트는 member->Click = ... 처럼 컨트롤에 직접 건다.
    template <typename T>
    bool Bind(int id, std::shared_ptr<T>& member) const {
        member = Get<T>(id);
        return member.get() != NULL;
    }

    void AddControl(const std::shared_ptr<Control>& c);
    void RemoveControl(Control* c);
    // 빌려서 쓴다. 소유권 없음. 수명은 호출자 책임. NULL이면 기본 스킨.
    void SetSkin(NinePatch* skin);
    NinePatch* GetSkin() const { return m_skin; } // NULL이면 기본 스킨을 쓰라는 뜻
    NinePatch* GetEffectiveSkin() const;          // 실제로 그리는 스킨

    // 자식 c의 자리에 해당하는 내 스킨 정보를 채운다.
    // 창이 아직 없으면(크기 0) 스킨 물리 크기로 그린다고 보고 계산한다.
    void GetChildPaintContext(const Control& c, PaintContext& ctx) const;

    // --- 채널 구독을 창 수명에 묶기 (UI 스레드) ---
    // 창이 닫히면(OnDestroy) 또는 Dialog가 파괴되면 자동으로 해제된다.
    // 자식 창은 데이터를 갖지 않고 부모/앱의 채널을 구독만 한다 → 닫혀도 데이터는 채널에 쌓인다.
    // DoModal 직전(또는 OnCreate)에 구독할 것. 다시 열면 다시 구독한다.
    void KeepSubscription(Subscription s);
    template <typename T>
    void Subscribe(const std::shared_ptr<Channel<T> >& ch,
            std::function<void(const T&)> handler) {
        if (ch) KeepSubscription(ch->Subscribe(handler));
    }
    void ClearSubscriptions();

protected:
    virtual void OnPaintLayered(Renderer& r, int w, int h);
    virtual bool OnLeftDown(int x, int y);
    virtual void OnLeftUp(int x, int y);
    virtual void OnMouseMove(int x, int y);
    virtual void OnMouseLeave();
    virtual void OnCaptureLost();
    virtual void OnKeyDown(unsigned vk);
    virtual void OnChar(wchar_t ch);
    virtual void OnDestroy();

    Control* ControlFromPoint(int x, int y);
    void LogBindMiss(int id, bool typeMismatch) const;
    std::shared_ptr<Control> FindShared(Control* c) const;

    std::vector<std::shared_ptr<Control> > m_controls;
    NinePatch* m_skin; // 빌린 것. 소유권 없음.
    bool m_modal;
    bool m_modalDone;
    int m_result;
    Control* m_pressed;              // 누른 컨트롤 (캡처 중)
    std::weak_ptr<Control> m_keyTarget; // WM_CHAR 대상 (포커스 구현 전 임시: 마지막으로 누른 것)
    std::vector<Subscription> m_subscriptions; // 창 수명에 묶인 구독
    std::vector<std::shared_ptr<Control> > m_templateControls; // 템플릿으로 만든 것 (다시 Load 시 교체)
    size_t m_skippedItems;
    std::wstring m_templateTitle;
    int m_templateW, m_templateH;
};

} // namespace nfc
