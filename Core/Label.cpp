#include "framework.h"
#include "Label.h"
#include <memory>

namespace nfc {

// DLL 안에 단일 인스턴스를 둔다 (헤더 인라인 static은 모듈별 복제 위험).
// App::Shutdown()에서 Label::Shutdown()으로 명시 해제.
namespace {
std::unique_ptr<Gdiplus::Font>& FontHolder() {
    static std::unique_ptr<Gdiplus::Font> s;
    return s;
}
} // namespace

Gdiplus::Font* Label::DefaultFont() {
    std::unique_ptr<Gdiplus::Font>& s = FontHolder();
    if (!s.get()) {
        s.reset(new Gdiplus::Font(L"Malgun Gothic", 9.0f,
            Gdiplus::FontStyleRegular, Gdiplus::UnitPixel));
    }
    return s.get();
}

void Label::Shutdown() {
    FontHolder().reset();
}

void Label::OnPaint(Gdiplus::Graphics& g, const PaintContext& ctx) {
    PaintSkin(g, ctx);
    DrawTextCentered(g, m_color, m_align);
}

} // namespace nfc
