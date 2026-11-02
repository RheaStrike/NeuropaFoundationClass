#pragma once
#include "CObject.h"
#include "SkinRegion.h"
#include <memory>
#include <string>

// framework.h를 먼저 포함하고 쓸 것 (HWND, Gdiplus 정의 필요)

namespace nfc {

class NinePatch;

enum RenderBackend {
    RENDER_GDIPLUS = 0, // 현재 유일한 구현
    RENDER_D2D,         // TODO: 심화 과정 (Direct2D)
    RENDER_D3D          // TODO: 심화 과정 (Direct3D)
};

// 레이어드 창 하나의 그리기 표면. 중심은 9패치(DrawNinePatch)와 글자(DrawString).
// 한 프레임 = BeginFrame → 그리기 → EndFrame → PresentLayered.
// 표면(DIB)은 크기가 바뀔 때만 다시 만든다. UI 스레드 전용.
class NFC_API Renderer : public CObject {
public:
    virtual ~Renderer() {}

    // 백엔드 생성. 아직 없는 백엔드(D2D/D3D)는 NULL을 돌려준다 (호출자가 GDI+로 대체).
    static std::unique_ptr<Renderer> Create(RenderBackend backend);

    virtual RenderBackend Backend() const = 0;

    // 투명으로 지운 w x h 표면을 준비. 실패하면 false (그리지 말 것).
    virtual bool BeginFrame(int w, int h) = 0;
    virtual void EndFrame() = 0;
    // 표면을 레이어드 창에 올린다 (premultiplied alpha, UpdateLayeredWindow).
    virtual bool PresentLayered(HWND hwnd) = 0;

    virtual void DrawNinePatch(const NinePatch& np, int x, int y, int w, int h) = 0;
    virtual void DrawNinePatchRegion(const NinePatch& np, int x, int y, int w, int h,
        const SkinRegion& src) = 0;
    // align: 0 왼쪽, 1 가운데, 2 오른쪽. argb = 0xAARRGGBB
    virtual void DrawString(const std::wstring& text, int x, int y, int w, int h,
        unsigned argb, int align) = 0;

    // GDI+ 호환 경로: 컨트롤은 아직 Gdiplus::Graphics로 그린다.
    // 프레임 안(BeginFrame~EndFrame)에서만 유효. GDI+가 아닌 백엔드는 NULL.
    virtual Gdiplus::Graphics* GdiplusGraphics() { return NULL; }
};

} // namespace nfc
