#pragma once

// 스킨 원본 영역 + 슬라이스. NinePatch와 컨트롤이 공유하는 값 타입. 의존성 없음.

namespace nfc {

struct SkinRegion {
    int x, y, w, h;               // 원본 영역
    int left, top, right, bottom; // 고정 테두리
};

} // namespace nfc
