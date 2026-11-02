#pragma once
#include "Control.h"
#include <functional>

// framework.h를 먼저 포함하고 쓸 것 (Gdiplus 정의 필요)

namespace nfc {

// 윈도우리스 버튼. 스킨 규칙은 Control과 같다.
// 기본값: 보통 상태 명시 영역 = 기본 스킨의 버튼 밴드 (NinePatch::ButtonRegion).
// ClearSkinRegion(SKIN_NORMAL)이면 부모가 알려 준 자기 자리 스킨을 쓴다 (NeuroCD식).
// 호버/누름/비활성은 반투명 틴트 + 테두리 색으로 표시 (PNG 한 장이면 충분).
class NFC_API Button : public Control {
public:
    Button();

    std::function<void()> Click;

    void SetTextColor(Gdiplus::Color c) { m_textColor = c; }

    virtual void OnMouseUp(int x, int y);

protected:
    virtual void OnPaint(Gdiplus::Graphics& g, const PaintContext& ctx);

private:
    Gdiplus::Color m_textColor;
};

} // namespace nfc
