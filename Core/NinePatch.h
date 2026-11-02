#pragma once
#include "CObject.h"
#include "SkinRegion.h"
#include <memory>
#include <string>

// framework.h를 먼저 포함하고 쓸 것 (Gdiplus 정의 필요)

namespace nfc {

// 9패치 이미지. PNG 파일(1px 마커 자동 인식) 또는 코드 생성 도형.
// 한 장의 스킨에서 영역(Region)만 잘라 쓰는 것도 지원한다.
// 모든 컨트롤·부모 창의 바탕. PNG 알파는 읽을 때 PixelFormat32bppPARGB(premultiplied)로
// 바꿔 두므로 UpdateLayeredWindow(ULW_ALPHA)에 그대로 합성된다 (BitBlt 아님, GDI+ 합성).
class NFC_API NinePatch : public CObject {
public:
    NinePatch();
    virtual ~NinePatch();
    NinePatch(const NinePatch& o); // 값 복사. 비트맵 공유, 슬라이스·영역 복사.
    NinePatch& operator=(const NinePatch& o);

    // GDI+로 PNG 읽기, 마커 자동 인식, PARGB로 변환 (파일 잠금은 바로 풀린다)
    bool Load(const std::wstring& path);
    // 고정 테두리 지정. SetStretch와 같은 것을 다르게 표현한 것으로 서로 변환된다.
    void SetSlices(int left, int top, int right, int bottom);
    bool IsValid() const { return m_bitmap.get() != NULL; }

    // 영역 스킨: 비트맵 안에서 (x,y,w,h)만 쓴다. 슬라이스는 영역 기준.
    void SetRegion(int x, int y, int w, int h);
    void ClearRegion();
    bool HasRegion() const { return m_hasRegion; }
    // 영역을 공유하는 새 NinePatch. 비트맵 공유, 슬라이스는 3등분.
    // 호출자가 소유. 영역이 스킨을 벗어나면 NULL.
    NinePatch* Sub(int x, int y, int w, int h) const;
    // 복사본. 비트맵 공유. 호출자가 소유.
    NinePatch* Clone() const;

    // 늘이는 영역 지정 (원본 영역 기준 상대 좌표, 끝점 제외).
    // 원본을 벗어나면 잘라낸다.
    void SetStretch(int sx0, int sy0, int sx1, int sy1);
    void GetStretch(int& sx0, int& sy0, int& sx1, int& sy1) const;
    void GetSlices(int& left, int& top, int& right, int& bottom) const;
    void GetSource(int& x, int& y, int& w, int& h) const; // 영역 없으면 전체

    // 고정 부분이 찌그러지지 않는 최소 그리기 크기. 리사이즈 하한으로 쓴다.
    int MinWidth() const;
    int MinHeight() const;

    int ImageWidth() const;  // 영역 있으면 영역 너비
    int ImageHeight() const; // 영역 있으면 영역 높이

    void Draw(Gdiplus::Graphics& g, int x, int y, int w, int h) const;
    // 영역 지정 1회 그리기. 객체 상태는 안 바꾼다. 컨트롤이 OnPaint에서 쓴다.
    void DrawRegion(Gdiplus::Graphics& g, int dx, int dy, int dw, int dh,
        const SkinRegion& r) const;

    // 9패치로 (0,0,drawW,drawH)에 그렸을 때, 그 안의 (x,y,w,h) 자리에 깔리는
    // 원본 영역을 구한다. 부모가 자식에게 "네 자리의 내 스킨"을 알려 줄 때 쓴다.
    // 결과 슬라이스는 0 (단순 늘이기). 영역이 비면 false.
    bool MapToSource(int x, int y, int w, int h, int drawW, int drawH,
        SkinRegion& out) const;

    // 코드로 만드는 기본 스킨 (둥근 사각형). 호출자가 소유.
    static NinePatch* CreateRoundRect(int w, int h, int radius,
        Gdiplus::Color fill, Gdiplus::Color border, float borderWidth);

    // 기본 스킨 (exe 폴더의 Skin/default.png). 없으면 NfcException.
    // 공유 캐시. 호출자가 파괴하지 말 것. App::Shutdown에서 해제.
    static NinePatch* Default();
    // 기본 스킨의 버튼 밴드 (실측 좌표). 컨트롤이 OnPaint에서 참조한다.
    static SkinRegion ButtonRegion();
    // GDI+ 캐시 해제. App::Shutdown에서 GdiplusShutdown 전에 호출.
    static void Shutdown();
    // 읽기 성공 시 새 객체(호출자가 소유), 실패 시 기본 스킨(공유, 삭제 금지).
    // 기본도 없으면 NfcException.
    static NinePatch* LoadOrDefault(const std::wstring& path);

private:
    std::shared_ptr<Gdiplus::Bitmap> m_bitmap; // Sub끼리 공유
    int m_left, m_top, m_right, m_bottom;
    int m_rx, m_ry, m_rw, m_rh;
    bool m_hasRegion;
};

} // namespace nfc
