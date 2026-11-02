#include "framework.h"
#include "Button.h"
#include "NinePatch.h"

namespace nfc {
using namespace Gdiplus;

Button::Button() : m_textColor(255, 32, 32, 32) {
    SkinRegion r = NinePatch::ButtonRegion();
    SetSkinRegion(SKIN_NORMAL, r.x, r.y, r.w, r.h, r.left, r.top, r.right, r.bottom);
    SetStateFeedback(true);
}

void Button::OnPaint(Graphics& g, const PaintContext& ctx) {
    PaintSkin(g, ctx);
    DrawTextCentered(g, m_textColor, 1);
}

void Button::OnMouseUp(int x, int y) {
    bool was = m_pressed;
    Control::OnMouseUp(x, y);
    if (was && m_enabled && HitTest(x, y) && Click) Click();
}

} // namespace nfc
