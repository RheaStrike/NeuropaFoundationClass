#pragma once
#include "Control.h"

// framework.h를 먼저 포함하고 쓸 것 (Gdiplus 정의 필요)

namespace nfc {

// 정적 텍스트. 윈도우리스. 스킨도 가질 수 있다 (SetSkin / SetSkinRegion).
// 스킨이 없으면 부모가 알려 준 자기 자리 스킨 위에 글자만 그린다.
class NFC_API Label : public Control {
public:
    Label() : m_color(255, 255, 255, 255), m_align(0) {}
    void SetTextColor(Gdiplus::Color c) { m_color = c; }
    void SetAlign(int a) { m_align = a; } // 0 왼쪽, 1 가운데, 2 오른쪽

    static Gdiplus::Font* DefaultFont();
    static void Shutdown(); // 폰트 캐시 해제. App::Shutdown에서 호출.

protected:
    virtual void OnPaint(Gdiplus::Graphics& g, const PaintContext& ctx);

private:
    Gdiplus::Color m_color;
    int m_align;
};

} // namespace nfc
