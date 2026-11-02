#include "framework.h"
#include "GdiRenderer.h"
#include "NinePatch.h"
#include "Label.h"

namespace nfc {
using namespace Gdiplus;

GdiRenderer::GdiRenderer()
    : m_dc(NULL), m_dib(NULL), m_oldBmp(NULL), m_bits(NULL), m_w(0), m_h(0) {
}

GdiRenderer::~GdiRenderer() {
    EndFrame();
    ReleaseSurface();
}

void GdiRenderer::ReleaseSurface() {
    if (m_dc && m_oldBmp) SelectObject(m_dc, m_oldBmp);
    m_oldBmp = NULL;
    if (m_dib) {
        DeleteObject(m_dib);
        m_dib = NULL;
    }
    if (m_dc) {
        DeleteDC(m_dc);
        m_dc = NULL;
    }
    m_bits = NULL;
    m_w = 0;
    m_h = 0;
}

bool GdiRenderer::EnsureSurface(int w, int h) {
    if (m_dib && w == m_w && h == m_h) return true; // 크기 같으면 재사용
    ReleaseSurface();
    HDC screen = GetDC(NULL);
    m_dc = CreateCompatibleDC(screen);
    ReleaseDC(NULL, screen);
    if (!m_dc) return false;
    BITMAPINFO bmi;
    ZeroMemory(&bmi, sizeof(bmi));
    bmi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bmi.bmiHeader.biWidth = w;
    bmi.bmiHeader.biHeight = -h; // 톱다운
    bmi.bmiHeader.biPlanes = 1;
    bmi.bmiHeader.biBitCount = 32;
    bmi.bmiHeader.biCompression = BI_RGB;
    m_dib = CreateDIBSection(m_dc, &bmi, DIB_RGB_COLORS, &m_bits, NULL, 0);
    if (!m_dib || !m_bits) {
        ReleaseSurface();
        return false;
    }
    m_oldBmp = SelectObject(m_dc, m_dib);
    m_w = w;
    m_h = h;
    return true;
}

bool GdiRenderer::BeginFrame(int w, int h) {
    EndFrame();
    if (w <= 0 || h <= 0 || !EnsureSurface(w, h)) return false;
    GdiFlush();
    // DIB 비트를 그대로 PARGB 비트맵으로 감싼다 (복사 없음)
    m_bitmap.reset(new Bitmap(w, h, w * 4, PixelFormat32bppPARGB,
        static_cast<BYTE*>(m_bits)));
    if (m_bitmap->GetLastStatus() != Ok) {
        m_bitmap.reset();
        return false;
    }
    m_graphics.reset(new Graphics(m_bitmap.get()));
    m_graphics->SetCompositingMode(CompositingModeSourceCopy);
    m_graphics->Clear(Color(0, 0, 0, 0));
    m_graphics->SetCompositingMode(CompositingModeSourceOver);
    m_graphics->SetCompositingQuality(CompositingQualityHighSpeed);
    m_graphics->SetSmoothingMode(SmoothingModeAntiAlias);
    m_graphics->SetPixelOffsetMode(PixelOffsetModeHalf);
    // 레이어드에서는 ClearType 금지 (알파 없는 서브픽셀이라 번짐)
    m_graphics->SetTextRenderingHint(TextRenderingHintAntiAlias);
    return true;
}

void GdiRenderer::EndFrame() {
    if (m_graphics.get()) m_graphics->Flush(FlushIntentionSync);
    m_graphics.reset();
    m_bitmap.reset();
}

bool GdiRenderer::PresentLayered(HWND hwnd) {
    if (!hwnd || !m_dc || m_graphics.get()) return false; // EndFrame 뒤에만
    HDC screen = GetDC(NULL);
    POINT src = { 0, 0 };
    SIZE sz = { m_w, m_h };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
    BOOL ok = UpdateLayeredWindow(hwnd, screen, NULL, &sz, m_dc, &src, 0, &bf, ULW_ALPHA);
    ReleaseDC(NULL, screen);
    return ok != FALSE;
}

void GdiRenderer::DrawNinePatch(const NinePatch& np, int x, int y, int w, int h) {
    if (m_graphics.get()) np.Draw(*m_graphics, x, y, w, h);
}

void GdiRenderer::DrawNinePatchRegion(const NinePatch& np, int x, int y, int w, int h,
        const SkinRegion& src) {
    if (m_graphics.get()) np.DrawRegion(*m_graphics, x, y, w, h, src);
}

void GdiRenderer::DrawString(const std::wstring& text, int x, int y, int w, int h,
        unsigned argb, int align) {
    if (!m_graphics.get() || text.empty()) return;
    SolidBrush b(Color((ARGB)argb));
    StringFormat fmt;
    fmt.SetAlignment(align == 1 ? StringAlignmentCenter :
        (align == 2 ? StringAlignmentFar : StringAlignmentNear));
    fmt.SetLineAlignment(StringAlignmentCenter);
    RectF rc((REAL)x, (REAL)y, (REAL)w, (REAL)h);
    m_graphics->DrawString(text.c_str(), -1, Label::DefaultFont(), rc, &fmt, &b);
}

} // namespace nfc
