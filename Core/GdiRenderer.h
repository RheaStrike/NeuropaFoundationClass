#pragma once
#include "Renderer.h"

// framework.h를 먼저 포함하고 쓸 것

namespace nfc {

// GDI+ 렌더러. 32bpp 톱다운 DIB 하나를 재사용하고(크기 바뀔 때만 재생성),
// 프레임마다 그 비트를 PixelFormat32bppPARGB로 감싼 Gdiplus::Bitmap에 그린다.
// → 결과가 곧 premultiplied alpha라 UpdateLayeredWindow(ULW_ALPHA)에 그대로 올린다.
// GDI+ 객체는 프레임 동안에만 살아 있으므로 GdiplusShutdown 이후 파괴돼도 안전하다.
class GdiRenderer : public Renderer {
public:
    GdiRenderer();
    virtual ~GdiRenderer();

    virtual RenderBackend Backend() const { return RENDER_GDIPLUS; }
    virtual bool BeginFrame(int w, int h);
    virtual void EndFrame();
    virtual bool PresentLayered(HWND hwnd);
    virtual void DrawNinePatch(const NinePatch& np, int x, int y, int w, int h);
    virtual void DrawNinePatchRegion(const NinePatch& np, int x, int y, int w, int h,
        const SkinRegion& src);
    virtual void DrawString(const std::wstring& text, int x, int y, int w, int h,
        unsigned argb, int align);
    virtual Gdiplus::Graphics* GdiplusGraphics() { return m_graphics.get(); }

private:
    bool EnsureSurface(int w, int h);
    void ReleaseSurface();

    HDC m_dc;
    HBITMAP m_dib;
    HGDIOBJ m_oldBmp;
    void* m_bits;
    int m_w, m_h;
    std::unique_ptr<Gdiplus::Bitmap> m_bitmap;     // 프레임 동안만
    std::unique_ptr<Gdiplus::Graphics> m_graphics; // 프레임 동안만
};

} // namespace nfc
