#include "framework.h"
#include "Dialog.h"
#include "NinePatch.h"
#include "App.h"

namespace nfc {
using namespace Gdiplus;

Dialog::Dialog()
    : m_skin(NULL), m_modal(false), m_modalDone(false),
      m_result(0), m_pressed(NULL), m_skippedItems(0), m_templateW(0), m_templateH(0) {
}

Dialog::~Dialog() {
    // 파생 부분이 살아 있을 때 창을 닫아 OnDestroy가 제대로 불리게 한다.
    // (Wnd::~Wnd까지 미루면 가상 콜백이 이미 파괴된 파생 부분으로 갈 수 있다.)
    if (m_hwnd) Close();
    ClearSubscriptions(); // 창을 만든 적 없어도 구독은 푼다
    // 살아남은 컨트롤의 부모를 끊는다 (윈도우 규칙: 자식은 부모 없이 못 산다)
    for (size_t i = 0; i < m_controls.size(); ++i) {
        m_controls[i]->SetParent(NULL);
    }
}

NinePatch* Dialog::GetEffectiveSkin() const {
    return m_skin ? m_skin : NinePatch::Default();
}

bool Dialog::Create(Wnd* parent, const wchar_t* title, int w, int h) {
    if (w <= 0 || h <= 0) {
        NinePatch* skin = GetEffectiveSkin();
        if (w <= 0) w = skin->ImageWidth();
        if (h <= 0) h = skin->ImageHeight();
        if (w <= 0) w = 320;
        if (h <= 0) h = 240;
    }
    return Wnd::Create(parent, title,
        (GetSystemMetrics(SM_CXSCREEN) - w) / 2,
        (GetSystemMetrics(SM_CYSCREEN) - h) / 2, w, h);
}

int Dialog::DoModal(Wnd* parent, const wchar_t* title, int w, int h) {
    if (!Create(parent, title, w, h)) return -1;
    if (parent && parent->GetHwnd()) EnableWindow(parent->GetHwnd(), FALSE);
    m_modal = true;
    m_modalDone = false;
    m_result = 0;
    Show();
    MSG msg;
    while (!m_modalDone) {
        int r = GetMessage(&msg, NULL, 0, 0);
        if (r == 0) {
            PostQuitMessage((int)msg.wParam); // 바깥 루프가 보게 WM_QUIT 다시 게시
            break;
        }
        if (r < 0) break; // 에러
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    m_modal = false;
    if (parent && parent->GetHwnd()) EnableWindow(parent->GetHwnd(), TRUE);
    if (parent && parent->GetHwnd()) SetForegroundWindow(parent->GetHwnd());
    Close();
    return m_result;
}

void Dialog::EndDialog(int result) {
    m_result = result;
    m_modalDone = true;
}

bool Dialog::LoadFromResource(HINSTANCE inst, UINT idd, int dpi) {
    DialogTemplate t;
    std::wstring err;
    if (!DialogTemplate::LoadFromResource(inst, idd, t, &err)) {
        DebugLog(L"IDD " + std::to_wstring(idd) + L" load failed: " + err);
        return false;
    }
    int bx = 0, by = 0;
    t.ComputeBaseUnits(dpi, bx, by);
    return LoadFromTemplate(t, bx, by);
}

bool Dialog::LoadFromTemplate(const void* data, size_t size, int dpi) {
    DialogTemplate t;
    std::wstring err;
    if (!DialogTemplate::Parse(data, size, t, &err)) {
        DebugLog(L"dialog template parse failed: " + err);
        return false;
    }
    int bx = 0, by = 0;
    t.ComputeBaseUnits(dpi, bx, by);
    return LoadFromTemplate(t, bx, by);
}

bool Dialog::LoadFromTemplate(const DialogTemplate& src, int baseX, int baseY) {
    App::AssertUiThread();
    DialogTemplate t = src;
    t.ApplyLayout(baseX > 0 ? baseX : 1, baseY > 0 ? baseY : 1);
    // 이전에 템플릿으로 만든 컨트롤만 뺀다 (코드로 AddControl한 것은 그대로)
    std::vector<std::shared_ptr<Control> > old;
    old.swap(m_templateControls);
    for (size_t i = 0; i < old.size(); ++i) RemoveControl(old[i].get());
    m_skippedItems = 0;
    m_templateTitle = t.title;
    m_templateW = t.pw;
    m_templateH = t.ph;
    for (size_t i = 0; i < t.items.size(); ++i) {
        const DialogItem& it = t.items[i];
        std::shared_ptr<Control> c = ControlRegistry::CreateControl(it);
        if (!c) { ++m_skippedItems; continue; }
        // 공통 규칙: 위치/크기/ID/캡션/보이기/활성. 스킨은 컨트롤 기본값(부모 영역 규칙) 그대로.
        c->SetBounds(it.px, it.py, it.pw, it.ph);
        c->SetId(it.id);
        c->SetText(it.text);
        c->SetVisible((it.style & WS_VISIBLE) != 0);
        c->SetEnabled((it.style & WS_DISABLED) == 0);
        AddControl(c);
        m_templateControls.push_back(c);
    }
    return true;
}

bool Dialog::CreateFromResource(HINSTANCE inst, UINT idd, Wnd* parent) {
    if (!LoadFromResource(inst, idd)) return false;
    return Create(parent, m_templateTitle.c_str(), m_templateW, m_templateH);
}

int Dialog::DoModal(Wnd* parent, HINSTANCE inst, UINT idd) {
    if (!LoadFromResource(inst, idd)) return -1;
    return DoModal(parent, m_templateTitle.c_str(), m_templateW, m_templateH);
}

std::shared_ptr<Control> Dialog::FindControl(int id) const {
    if (id == 0 || id == -1) return std::shared_ptr<Control>(); // 없음 / IDC_STATIC
    for (size_t i = 0; i < m_controls.size(); ++i) {
        if (m_controls[i]->GetId() == id) return m_controls[i];
    }
    return std::shared_ptr<Control>();
}

void Dialog::LogBindMiss(int id, bool typeMismatch) const {
    DebugLog(std::wstring(typeMismatch ? L"bind: type mismatch for id " : L"bind: no control with id ") +
        std::to_wstring(id));
}

void Dialog::AddControl(const std::shared_ptr<Control>& c) {
    if (!c.get()) return;
    Dialog* old = c->GetParent();
    if (old == this) return; // 이미 소속. 중복 등록 금지.
    if (old) old->RemoveControl(c.get()); // 윈도우 규칙: 부모는 하나. 옮기면 뺏는다.
    c->SetParent(this);
    m_controls.push_back(c);
    Invalidate();
}

void Dialog::RemoveControl(Control* c) {
    if (!c) return;
    for (size_t i = 0; i < m_controls.size(); ++i) {
        if (m_controls[i].get() == c) {
            if (m_pressed == c) m_pressed = NULL;
            if (m_keyTarget.lock().get() == c) m_keyTarget.reset();
            m_controls[i]->SetParent(NULL);
            m_controls.erase(m_controls.begin() + i);
            Invalidate();
            return;
        }
    }
}

void Dialog::SetSkin(NinePatch* skin) {
    m_skin = skin;
    Invalidate();
}

void Dialog::GetChildPaintContext(const Control& c, PaintContext& ctx) const {
    NinePatch* skin = GetEffectiveSkin();
    int dw = m_w > 0 ? m_w : skin->ImageWidth();
    int dh = m_h > 0 ? m_h : skin->ImageHeight();
    ctx.skin = skin;
    ctx.dx = c.GetX();
    ctx.dy = c.GetY();
    ctx.dw = c.GetWidth();
    ctx.dh = c.GetHeight();
    ctx.hasSource = skin->MapToSource(c.GetX(), c.GetY(), c.GetWidth(), c.GetHeight(),
        dw, dh, ctx.src);
}

void Dialog::OnPaintLayered(Renderer& r, int w, int h) {
    NinePatch* skin = GetEffectiveSkin();
    r.DrawNinePatch(*skin, 0, 0, w, h);
    Graphics* g = r.GdiplusGraphics();
    if (!g) return; // TODO: GDI+가 아닌 백엔드용 컨트롤 그리기 경로
    for (size_t i = 0; i < m_controls.size(); ++i) {
        Control* c = m_controls[i].get();
        if (!c->IsVisible()) continue;
        PaintContext ctx;
        GetChildPaintContext(*c, ctx);
        c->Paint(*g, ctx);
    }
}

Control* Dialog::ControlFromPoint(int x, int y) {
    for (size_t i = m_controls.size(); i > 0; --i) {
        Control* c = m_controls[i - 1].get();
        if (c->IsEnabled() && c->HitTest(x, y)) return c;
    }
    return NULL;
}

std::shared_ptr<Control> Dialog::FindShared(Control* c) const {
    for (size_t i = 0; i < m_controls.size(); ++i) {
        if (m_controls[i].get() == c) return m_controls[i];
    }
    return std::shared_ptr<Control>();
}

bool Dialog::OnLeftDown(int x, int y) {
    Control* c = ControlFromPoint(x, y);
    if (!c) return false; // 빈 곳 = 드래그 (Wnd 기본 동작)
    m_pressed = c;
    m_keyTarget = FindShared(c);
    c->OnMouseDown(x, y);
    return true; // Wnd가 마우스를 캡처한다
}

void Dialog::OnLeftUp(int x, int y) {
    if (m_pressed) {
        // Click 안에서 컨트롤이 제거돼도 안전하게 잠깐 붙잡는다
        std::shared_ptr<Control> c = FindShared(m_pressed);
        m_pressed = NULL;
        if (c.get()) c->OnMouseUp(x, y);
    }
}

void Dialog::OnMouseMove(int x, int y) {
    for (size_t i = 0; i < m_controls.size(); ++i) {
        m_controls[i]->OnMouseMove(x, y);
    }
}

void Dialog::OnMouseLeave() {
    for (size_t i = 0; i < m_controls.size(); ++i) {
        m_controls[i]->OnMouseLeave();
    }
}

void Dialog::OnCaptureLost() {
    if (m_pressed) {
        Control* c = m_pressed;
        m_pressed = NULL;
        c->OnCaptureLost();
    }
}

void Dialog::OnKeyDown(unsigned vk) {
    if (vk == VK_ESCAPE) EndDialog(IDCANCEL);
    else if (vk == VK_RETURN) EndDialog(IDOK);
}

void Dialog::OnChar(wchar_t ch) {
    std::shared_ptr<Control> c = m_keyTarget.lock();
    if (c.get() && c->IsEnabled() && c->IsVisible()) c->OnChar(ch);
}

void Dialog::KeepSubscription(Subscription s) {
    App::AssertUiThread();
    if (s.IsActive()) m_subscriptions.push_back(std::move(s));
}

void Dialog::ClearSubscriptions() {
    // 목록을 먼저 떼어 낸 뒤 푼다 (해제 중 재진입 대비)
    std::vector<Subscription> subs;
    subs.swap(m_subscriptions);
    subs.clear();
}

void Dialog::OnDestroy() {
    ClearSubscriptions(); // 닫히면 구독 해제. 이후 데이터는 채널에 쌓인다
    m_modalDone = true; // 모달 루프 탈출 보장
    m_pressed = NULL;
}

} // namespace nfc
