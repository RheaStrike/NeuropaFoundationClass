#include "framework.h"
#include "Control.h"
#include "Dialog.h"
#include "Label.h"
#include "NinePatch.h"

namespace nfc {
using namespace Gdiplus;

Control::Control() : m_x(0), m_y(0), m_w(0), m_h(0),
        m_visible(true), m_enabled(true), m_hover(false), m_pressed(false),
        m_useParentSkin(true), m_stateFeedback(false), m_id(0), m_parent(nullptr) {
    for (int i = 0; i < SKIN_STATE_COUNT; ++i) {
        m_skins[i] = NULL;
        m_hasRegion[i] = false;
        SkinRegion z = { 0, 0, 0, 0, 0, 0, 0, 0 };
        m_regions[i] = z;
    }
}

Control::~Control() {
}

void Control::SetEnabled(bool e) {
    if (m_enabled == e) return;
    m_enabled = e;
    if (!e) m_pressed = false;
    Invalidate();
}

void Control::SetSkin(NinePatch* skin, int state) {
    if (state < 0 || state >= SKIN_STATE_COUNT) return;
    m_skins[state] = skin;
}

NinePatch* Control::GetSkin(int state) const {
    if (state < 0 || state >= SKIN_STATE_COUNT) return NULL;
    return m_skins[state];
}

void Control::SetSkinRegion(int state, int x, int y, int w, int h,
        int l, int t, int r, int b) {
    if (state < 0 || state >= SKIN_STATE_COUNT) return;
    SkinRegion s = { x, y, w, h, l, t, r, b };
    m_regions[state] = s;
    m_hasRegion[state] = true;
}

void Control::ClearSkinRegion(int state) {
    if (state < 0 || state >= SKIN_STATE_COUNT) return;
    m_hasRegion[state] = false;
}

int Control::GetState() const {
    if (!m_enabled) return SKIN_DISABLED;
    if (m_pressed) return SKIN_PRESSED;
    if (m_hover) return SKIN_HOVER;
    return SKIN_NORMAL;
}

void Control::Paint(Graphics& g) {
    PaintContext ctx;
    ctx.skin = NULL;
    ctx.hasSource = false;
    ctx.dx = m_x;
    ctx.dy = m_y;
    ctx.dw = m_w;
    ctx.dh = m_h;
    if (m_parent) m_parent->GetChildPaintContext(*this, ctx);
    Paint(g, ctx);
}

void Control::Paint(Graphics& g, const PaintContext& ctx) {
    if (!m_visible || m_w <= 0 || m_h <= 0) return;
    OnPaint(g, ctx);
}

void Control::OnPaint(Graphics& g, const PaintContext& ctx) {
    PaintSkin(g, ctx);
}

void Control::PaintSkin(Graphics& g, const PaintContext& ctx) {
    const int st = GetState();
    NinePatch* own = m_skins[st] ? m_skins[st] : m_skins[SKIN_NORMAL];
    const SkinRegion* reg = m_hasRegion[st] ? &m_regions[st] :
        (m_hasRegion[SKIN_NORMAL] ? &m_regions[SKIN_NORMAL] : NULL);
    // 상태 전용 스킨/영역이 있으면 그걸로 상태를 표현하므로 틴트를 생략
    const bool stateSpecific = st != SKIN_NORMAL && (m_skins[st] || m_hasRegion[st]);

    if (own) {
        if (reg) own->DrawRegion(g, m_x, m_y, m_w, m_h, *reg);
        else own->Draw(g, m_x, m_y, m_w, m_h);
    } else if (reg) {
        NinePatch* skin = ctx.skin ? ctx.skin : NinePatch::Default();
        skin->DrawRegion(g, m_x, m_y, m_w, m_h, *reg);
    } else if (m_useParentSkin && ctx.skin && ctx.hasSource) {
        // 부모가 이미 같은 픽셀을 깔았으므로 덮어쓰기(SourceCopy)로 그린다.
        // 반투명 부분을 두 번 합성해 진해지는 것을 막는다.
        CompositingMode old = g.GetCompositingMode();
        g.SetCompositingMode(CompositingModeSourceCopy);
        ctx.skin->DrawRegion(g, ctx.dx, ctx.dy, ctx.dw, ctx.dh, ctx.src);
        g.SetCompositingMode(old);
    }
    if (m_stateFeedback && !stateSpecific) PaintStateOverlay(g, st);
}

namespace {
void AddRoundRect(GraphicsPath& path, REAL x, REAL y, REAL w, REAL h, REAL r) {
    REAL d = r * 2.0f;
    if (d > w) d = w;
    if (d > h) d = h;
    path.AddArc(x, y, d, d, 180.0f, 90.0f);
    path.AddArc(x + w - d, y, d, d, 270.0f, 90.0f);
    path.AddArc(x + w - d, y + h - d, d, d, 0.0f, 90.0f);
    path.AddArc(x, y + h - d, d, d, 90.0f, 90.0f);
    path.CloseFigure();
}
} // namespace

void Control::PaintStateOverlay(Graphics& g, int state) {
    if (m_w < 3 || m_h < 3) return;
    // 틴트: 호버 26, 누름 48 (Win11 파랑). 비활성은 옅은 회색 막.
    Color tint(0, 0, 0, 0);
    if (state == SKIN_PRESSED) tint = Color(48, 0x00, 0x78, 0xD7);
    else if (state == SKIN_HOVER) tint = Color(26, 0x00, 0x78, 0xD7);
    else if (state == SKIN_DISABLED) tint = Color(96, 0xF0, 0xF0, 0xF0);
    if (tint.GetA() != 0) {
        GraphicsPath clip;
        AddRoundRect(clip, (REAL)m_x + 1.0f, (REAL)m_y + 1.0f,
            (REAL)m_w - 2.0f, (REAL)m_h - 2.0f, 3.0f);
        SolidBrush b(tint);
        g.FillPath(&b, &clip);
    }
    // 테두리: 얇은 둥근 사각형. 상태별 색
    Color edge(255, 0xAD, 0xAD, 0xAD);
    if (state == SKIN_DISABLED) edge = Color(255, 0xBF, 0xBF, 0xBF);
    else if (state == SKIN_PRESSED) edge = Color(255, 0x00, 0x54, 0xA8);
    else if (state == SKIN_HOVER) edge = Color(255, 0x00, 0x78, 0xD7);
    GraphicsPath frame;
    AddRoundRect(frame, (REAL)m_x + 0.5f, (REAL)m_y + 0.5f,
        (REAL)m_w - 1.0f, (REAL)m_h - 1.0f, 4.0f);
    Pen pen(edge, 1.0f);
    g.DrawPath(&pen, &frame);
}

Color Control::StateTextColor(Color normal) const {
    if (m_enabled) return normal;
    return Color((BYTE)(normal.GetA() / 2), 0x6D, 0x6D, 0x6D);
}

void Control::DrawTextCentered(Graphics& g, Color c, int align) {
    if (m_text.empty()) return;
    SolidBrush b(StateTextColor(c));
    StringFormat fmt;
    fmt.SetAlignment(align == 1 ? StringAlignmentCenter :
        (align == 2 ? StringAlignmentFar : StringAlignmentNear));
    fmt.SetLineAlignment(StringAlignmentCenter);
    REAL off = (m_stateFeedback && m_pressed) ? 1.0f : 0.0f; // 누르면 1px 밀림
    RectF rc((REAL)m_x + off, (REAL)m_y + off, (REAL)m_w, (REAL)m_h);
    g.DrawString(m_text.c_str(), -1, Label::DefaultFont(), rc, &fmt, &b);
}

void Control::OnMouseDown(int x, int y) {
    (void)x; (void)y;
    if (!m_enabled) return;
    m_pressed = true;
    if (m_stateFeedback) Invalidate();
}

void Control::OnMouseUp(int x, int y) {
    (void)x; (void)y;
    if (!m_pressed) return;
    m_pressed = false;
    if (m_stateFeedback) Invalidate();
}

void Control::OnMouseMove(int x, int y) {
    bool hover = m_enabled && HitTest(x, y);
    if (hover != m_hover) {
        m_hover = hover;
        if (m_stateFeedback) Invalidate();
    }
}

void Control::OnMouseLeave() {
    if (m_hover) {
        m_hover = false;
        if (m_stateFeedback) Invalidate();
    }
}

void Control::OnCaptureLost() {
    if (m_pressed) {
        m_pressed = false;
        if (m_stateFeedback) Invalidate();
    }
}

void Control::OnChar(wchar_t ch) {
    (void)ch;
}

void Control::Invalidate() {
    if (m_parent) m_parent->Invalidate();
}

} // namespace nfc
