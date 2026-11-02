#include "framework.h"
#include "NinePatch.h"

namespace nfc {
using namespace Gdiplus;

NinePatch::NinePatch()
    : m_left(0), m_top(0), m_right(0), m_bottom(0),
      m_rx(0), m_ry(0), m_rw(0), m_rh(0), m_hasRegion(false) {
}

NinePatch::~NinePatch() {
}

NinePatch::NinePatch(const NinePatch& o) {
    m_bitmap = o.m_bitmap;
    m_left = o.m_left;
    m_top = o.m_top;
    m_right = o.m_right;
    m_bottom = o.m_bottom;
    m_rx = o.m_rx;
    m_ry = o.m_ry;
    m_rw = o.m_rw;
    m_rh = o.m_rh;
    m_hasRegion = o.m_hasRegion;
}

NinePatch& NinePatch::operator=(const NinePatch& o) {
    if (this != &o) {
        m_bitmap = o.m_bitmap;
        m_left = o.m_left;
        m_top = o.m_top;
        m_right = o.m_right;
        m_bottom = o.m_bottom;
        m_rx = o.m_rx;
        m_ry = o.m_ry;
        m_rw = o.m_rw;
        m_rh = o.m_rh;
        m_hasRegion = o.m_hasRegion;
    }
    return *this;
}

namespace {
// src의 rc 부분을 PixelFormat32bppPARGB(premultiplied) 새 비트맵으로 복사한다.
// LockBits가 형식 변환을 해 주므로 픽셀이 정확히 보존된다.
// 결과는 파일과 무관하므로 Bitmap::FromFile의 파일 잠금도 여기서 풀린다.
Bitmap* ToPARGB(Bitmap* src, const Rect& rc) {
    if (rc.Width <= 0 || rc.Height <= 0) return NULL;
    std::unique_ptr<Bitmap> dst(new Bitmap(rc.Width, rc.Height, PixelFormat32bppPARGB));
    if (dst->GetLastStatus() != Ok) return NULL;
    Rect all(0, 0, rc.Width, rc.Height);
    BitmapData sd, dd;
    Rect srcRc = rc;
    if (src->LockBits(&srcRc, ImageLockModeRead, PixelFormat32bppPARGB, &sd) != Ok)
        return NULL;
    if (dst->LockBits(&all, ImageLockModeWrite, PixelFormat32bppPARGB, &dd) != Ok) {
        src->UnlockBits(&sd);
        return NULL;
    }
    for (INT y = 0; y < rc.Height; ++y) {
        memcpy(static_cast<BYTE*>(dd.Scan0) + (size_t)y * dd.Stride,
            static_cast<const BYTE*>(sd.Scan0) + (size_t)y * sd.Stride,
            (size_t)rc.Width * 4);
    }
    dst->UnlockBits(&dd);
    src->UnlockBits(&sd);
    return dst.release();
}

bool IsMarkerPixel(Bitmap* bmp, UINT x, UINT y) {
    Color c;
    if (bmp->GetPixel(x, y, &c) != Ok) return false;
    return c.GetA() == 255 && c.GetR() == 0 && c.GetG() == 0 && c.GetB() == 0;
}

// 1px 테두리 마커 → 잘라낼 영역(crop)과 늘이는 구간(마커 끝점 포함, crop 기준).
bool DetectMarkers(Bitmap* bmp, Rect& crop, int& sx0, int& sy0, int& sx1, int& sy1) {
    UINT w = bmp->GetWidth();
    UINT h = bmp->GetHeight();
    crop = Rect(0, 0, (INT)w, (INT)h);
    sx0 = sx1 = sy0 = sy1 = -1;
    if (w <= 2 || h <= 2) return false;
    for (UINT x = 1; x + 1 < w; ++x) {
        if (IsMarkerPixel(bmp, x, 0)) {
            if (sx0 < 0) sx0 = (int)x;
            sx1 = (int)x;
        }
    }
    for (UINT y = 1; y + 1 < h; ++y) {
        if (IsMarkerPixel(bmp, 0, y)) {
            if (sy0 < 0) sy0 = (int)y;
            sy1 = (int)y;
        }
    }
    if (sx0 < 0 || sy0 < 0) return false;
    crop = Rect(1, 1, (INT)w - 2, (INT)h - 2); // 마커 테두리를 잘라낸다
    sx0--;
    sy0--;
    sx1--;
    sy1--;
    return true;
}
} // namespace

bool NinePatch::Load(const std::wstring& path) {
    std::unique_ptr<Bitmap> src(Bitmap::FromFile(path.c_str(), FALSE));
    if (!src.get() || src->GetLastStatus() != Ok) return false;
    Rect crop;
    int sx0, sy0, sx1, sy1;
    bool markers = DetectMarkers(src.get(), crop, sx0, sy0, sx1, sy1);
    std::unique_ptr<Bitmap> pargb(ToPARGB(src.get(), crop));
    if (!pargb.get()) return false;
    m_bitmap = std::move(pargb);
    ClearRegion();
    if (markers) {
        SetStretch(sx0, sy0, sx1 + 1, sy1 + 1); // 마커는 끝점 포함
    } else {
        int w = ImageWidth(), h = ImageHeight();
        SetSlices(w / 3, h / 3, w / 3, h / 3); // 마커 없으면 3등분
    }
    return true;
}

void NinePatch::SetSlices(int left, int top, int right, int bottom) {
    m_left = left < 0 ? 0 : left;
    m_top = top < 0 ? 0 : top;
    m_right = right < 0 ? 0 : right;
    m_bottom = bottom < 0 ? 0 : bottom;
}

void NinePatch::SetStretch(int sx0, int sy0, int sx1, int sy1) {
    int sw = ImageWidth();
    int sh = ImageHeight();
    if (sx0 < 0) sx0 = 0;
    if (sy0 < 0) sy0 = 0;
    if (sx1 < sx0) sx1 = sx0;
    if (sy1 < sy0) sy1 = sy0;
    if (sx0 > sw) sx0 = sw;
    if (sy0 > sh) sy0 = sh;
    if (sx1 > sw) sx1 = sw;
    if (sy1 > sh) sy1 = sh;
    SetSlices(sx0, sy0, sw - sx1, sh - sy1);
}

void NinePatch::GetStretch(int& sx0, int& sy0, int& sx1, int& sy1) const {
    int sw = ImageWidth();
    int sh = ImageHeight();
    sx0 = m_left;
    sy0 = m_top;
    sx1 = sw - m_right;
    sy1 = sh - m_bottom;
    if (sx1 < sx0) sx1 = sx0;
    if (sy1 < sy0) sy1 = sy0;
}

void NinePatch::GetSlices(int& left, int& top, int& right, int& bottom) const {
    left = m_left;
    top = m_top;
    right = m_right;
    bottom = m_bottom;
}

void NinePatch::GetSource(int& x, int& y, int& w, int& h) const {
    if (m_hasRegion) {
        x = m_rx;
        y = m_ry;
        w = m_rw;
        h = m_rh;
        return;
    }
    x = 0;
    y = 0;
    w = ImageWidth();
    h = ImageHeight();
}

int NinePatch::MinWidth() const {
    return m_left + m_right;
}

int NinePatch::MinHeight() const {
    return m_top + m_bottom;
}

void NinePatch::SetRegion(int x, int y, int w, int h) {
    m_rx = x;
    m_ry = y;
    m_rw = w;
    m_rh = h;
    m_hasRegion = true;
}

void NinePatch::ClearRegion() {
    m_hasRegion = false;
}

NinePatch* NinePatch::Sub(int x, int y, int w, int h) const {
    Bitmap* bmp = m_bitmap.get();
    if (!bmp || w <= 0 || h <= 0) return NULL;
    int px = m_hasRegion ? m_rx : 0;
    int py = m_hasRegion ? m_ry : 0;
    int pw = m_hasRegion ? m_rw : (int)bmp->GetWidth();
    int ph = m_hasRegion ? m_rh : (int)bmp->GetHeight();
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x + w > pw) w = pw - x;
    if (y + h > ph) h = ph - y;
    if (w <= 0 || h <= 0) return NULL;
    std::unique_ptr<NinePatch> np(new NinePatch());
    np->m_bitmap = m_bitmap;
    np->SetRegion(px + x, py + y, w, h);
    int s = w / 3;
    int t = h / 3;
    if (s < 1) s = 1;
    if (t < 1) t = 1;
    np->SetSlices(s, t, s, t);
    return np.release();
}

NinePatch* NinePatch::Clone() const {
    return new NinePatch(*this);
}

int NinePatch::ImageWidth() const {
    if (m_hasRegion) return m_rw;
    return m_bitmap.get() ? (int)m_bitmap->GetWidth() : 0;
}

int NinePatch::ImageHeight() const {
    if (m_hasRegion) return m_rh;
    return m_bitmap.get() ? (int)m_bitmap->GetHeight() : 0;
}

// 조각 하나. TileFlipXY로 늘일 때 바깥(투명) 픽셀이 섞여 가장자리가 번지는 것을 막는다.
static void DrawPart(Graphics& g, Bitmap* bmp, ImageAttributes* attr,
        int dx, int dy, int dw, int dh, int sx, int sy, int sw, int sh) {
    if (dw <= 0 || dh <= 0 || sw <= 0 || sh <= 0) return;
    g.DrawImage(bmp, Rect(dx, dy, dw, dh), sx, sy, sw, sh, UnitPixel, attr);
}

void NinePatch::Draw(Graphics& g, int x, int y, int w, int h) const {
    SkinRegion r;
    if (m_hasRegion) {
        r.x = m_rx;
        r.y = m_ry;
        r.w = m_rw;
        r.h = m_rh;
    } else {
        r.x = 0;
        r.y = 0;
        r.w = m_bitmap.get() ? (int)m_bitmap->GetWidth() : 0;
        r.h = m_bitmap.get() ? (int)m_bitmap->GetHeight() : 0;
    }
    r.left = m_left;
    r.top = m_top;
    r.right = m_right;
    r.bottom = m_bottom;
    DrawRegion(g, x, y, w, h, r);
}

void NinePatch::DrawRegion(Graphics& g, int dx, int dy, int dw, int dh,
        const SkinRegion& r) const {
    Bitmap* bmp = m_bitmap.get();
    if (!bmp || dw <= 0 || dh <= 0 || r.w <= 0 || r.h <= 0) return;
    int ox = r.x, oy = r.y, bw = r.w, bh = r.h;
    int l0 = r.left, r0 = r.right, t0 = r.top, b0 = r.bottom;
    if (l0 < 0) l0 = 0;
    if (r0 < 0) r0 = 0;
    if (t0 < 0) t0 = 0;
    if (b0 < 0) b0 = 0;
    if (l0 > bw) l0 = bw;
    if (r0 > bw) r0 = bw;
    if (t0 > bh) t0 = bh;
    if (b0 > bh) b0 = bh;
    int l = l0, rgt = r0, t = t0, b = b0;
    // 목표가 고정폭보다 작으면 비율 축소
    if (l + rgt > dw && l + rgt > 0) {
        float s = (float)dw / (float)(l + rgt);
        l = (int)(l * s);
        rgt = dw - l;
    }
    if (t + b > dh && t + b > 0) {
        float s = (float)dh / (float)(t + b);
        t = (int)(t * s);
        b = dh - t;
    }
    int cx = dw - l - rgt;
    int cy = dh - t - b;
    ImageAttributes attr;
    attr.SetWrapMode(WrapModeTileFlipXY);
    DrawPart(g, bmp, &attr, dx, dy, l, t, ox, oy, l0, t0);
    DrawPart(g, bmp, &attr, dx + l, dy, cx, t, ox + l0, oy, bw - l0 - r0, t0);
    DrawPart(g, bmp, &attr, dx + l + cx, dy, rgt, t, ox + bw - r0, oy, r0, t0);
    DrawPart(g, bmp, &attr, dx, dy + t, l, cy, ox, oy + t0, l0, bh - t0 - b0);
    DrawPart(g, bmp, &attr, dx + l, dy + t, cx, cy, ox + l0, oy + t0, bw - l0 - r0, bh - t0 - b0);
    DrawPart(g, bmp, &attr, dx + l + cx, dy + t, rgt, cy, ox + bw - r0, oy + t0, r0, bh - t0 - b0);
    DrawPart(g, bmp, &attr, dx, dy + t + cy, l, b, ox, oy + bh - b0, l0, b0);
    DrawPart(g, bmp, &attr, dx + l, dy + t + cy, cx, b, ox + l0, oy + bh - b0, bw - l0 - r0, b0);
    DrawPart(g, bmp, &attr, dx + l + cx, dy + t + cy, rgt, b, ox + bw - r0, oy + bh - b0, r0, b0);
}

namespace {
// DrawRegion과 같은 규칙으로 고정폭을 줄인 뒤, 그린 좌표 v를 원본 좌표로 되돌린다.
// 마디: 그림 0, a, dst-b, dst ↔ 원본 0, a0, len-b0, len (구간별 선형)
int MapAxis(int v, int dst, int origin, int len, int a0, int b0) {
    if (a0 < 0) a0 = 0;
    if (b0 < 0) b0 = 0;
    if (a0 > len) a0 = len;
    if (b0 > len) b0 = len;
    int a = a0, b = b0;
    if (a + b > dst && a + b > 0) {
        float s = (float)dst / (float)(a + b);
        a = (int)(a * s);
        b = dst - a;
    }
    if (v <= 0) return origin;
    if (v >= dst) return origin + len;
    long long r;
    if (v < a) {
        r = (long long)v * a0 / a;
    } else if (v <= dst - b) {
        int c = dst - a - b;
        int mid = len - a0 - b0;
        r = a0 + (c > 0 ? (long long)(v - a) * mid / c : 0);
    } else {
        r = (len - b0) + (long long)(v - (dst - b)) * b0 / b;
    }
    return origin + (int)r;
}
} // namespace

bool NinePatch::MapToSource(int x, int y, int w, int h, int drawW, int drawH,
        SkinRegion& out) const {
    if (!m_bitmap.get() || w <= 0 || h <= 0 || drawW <= 0 || drawH <= 0) return false;
    int sx, sy, sw, sh;
    GetSource(sx, sy, sw, sh);
    if (sw <= 0 || sh <= 0) return false;
    int x0 = MapAxis(x, drawW, sx, sw, m_left, m_right);
    int x1 = MapAxis(x + w, drawW, sx, sw, m_left, m_right);
    int y0 = MapAxis(y, drawH, sy, sh, m_top, m_bottom);
    int y1 = MapAxis(y + h, drawH, sy, sh, m_top, m_bottom);
    out.x = x0;
    out.y = y0;
    out.w = x1 - x0;
    out.h = y1 - y0;
    out.left = out.top = out.right = out.bottom = 0;
    return out.w > 0 && out.h > 0;
}

NinePatch* NinePatch::CreateRoundRect(int w, int h, int radius,
        Color fill, Color border, float borderWidth) {
    if (w < 1) w = 1;
    if (h < 1) h = 1;
    if (radius < 1) radius = 1;
    std::unique_ptr<Bitmap> bmp(new Bitmap(w, h, PixelFormat32bppPARGB));
    Graphics g(bmp.get());
    g.Clear(Color(0, 0, 0, 0));
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    GraphicsPath path;
    int d = radius * 2;
    float iw = (float)w;
    float ih = (float)h;
    float inset = borderWidth * 0.5f;
    path.AddArc(inset, inset, (REAL)d, (REAL)d, 180.0f, 90.0f);
    path.AddArc(iw - d - inset, inset, (REAL)d, (REAL)d, 270.0f, 90.0f);
    path.AddArc(iw - d - inset, ih - d - inset, (REAL)d, (REAL)d, 0.0f, 90.0f);
    path.AddArc(inset, ih - d - inset, (REAL)d, (REAL)d, 90.0f, 90.0f);
    path.CloseFigure();
    SolidBrush fb(fill);
    g.FillPath(&fb, &path);
    if (borderWidth > 0.0f) {
        Pen pen(border, borderWidth);
        g.DrawPath(&pen, &path);
    }
    std::unique_ptr<NinePatch> np(new NinePatch());
    np->m_bitmap = std::move(bmp);
    np->SetSlices(radius, radius, radius, radius);
    return np.release();
}

static std::wstring ExeDir() {
    wchar_t buf[MAX_PATH];
    DWORD n = GetModuleFileNameW(NULL, buf, MAX_PATH);
    std::wstring s(buf, n > 0 ? n : 0);
    size_t p = s.find_last_of(L"\\/");
    if (p != std::wstring::npos) s.resize(p);
    return s;
}

// GDI+ 캐시 단일체. App::Shutdown()에서 NinePatch::Shutdown()으로 명시 해제.
// (GdiplusShutdown 이후에 파괴되면 AV. 정적 소멸 순서에 맡기지 않는다.)
namespace {
std::unique_ptr<NinePatch>& DefaultHolder() {
    static std::unique_ptr<NinePatch> s;
    return s;
}
} // namespace

NinePatch* NinePatch::Default() {
    std::unique_ptr<NinePatch>& s = DefaultHolder();
    if (!s.get()) {
        std::unique_ptr<NinePatch> np(new NinePatch());
        if (!np->Load(ExeDir() + L"\\Skin\\default.png"))
            throw NfcException("Default skin not found: Skin/default.png");
        s = std::move(np);
    }
    return s.get();
}

SkinRegion NinePatch::ButtonRegion() {
    // y=130부터 푸터(구분선+주황)라 깨끗한 92..123 밴드 사용.
    // 가로는 첫 패널 안쪽(x 48..65)만 쓴다. 예전처럼 8..341 전체를 쓰면
    // 패널 경계선(x≈115, 231)과 패널별 밝기 차이가 가운데 조각으로 압축돼
    // 버튼 글자 뒤에 다른 색 사각형처럼 보였다. 가운데는 2px만 늘여 면을 균일하게.
    SkinRegion r = { 48, 92, 18, 32, 8, 8, 8, 8 };
    return r;
}

void NinePatch::Shutdown() {
    DefaultHolder().reset();
}

NinePatch* NinePatch::LoadOrDefault(const std::wstring& path) {
    std::unique_ptr<NinePatch> np(new NinePatch());
    if (np->Load(path)) return np.release();
    return Default();
}

} // namespace nfc
