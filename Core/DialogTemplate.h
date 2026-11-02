#pragma once
#include "NFC_API.h"
#include <functional>
#include <memory>
#include <string>
#include <vector>

// framework.h를 먼저 포함하고 쓸 것 (WORD/DWORD/HINSTANCE 필요)

namespace nfc {

class Control;

// IDD_ 대화상자 리소스(DLGTEMPLATE / DLGTEMPLATEEX)를 직접 읽은 결과.
// 좌표는 대화상자 단위(DLU) 원본과, ApplyLayout 뒤의 픽셀 값을 함께 가진다.
// UI 스레드 전용 (파서 자체는 순수 함수라 어디서 불러도 되지만 규칙상 UI 스레드에서 쓴다).

// 표준 클래스 atom (sz_Or_Ord 0xFFFF 다음 WORD)
enum DialogClassAtom {
    DLG_ATOM_NONE = 0,
    DLG_ATOM_BUTTON = 0x0080,
    DLG_ATOM_EDIT = 0x0081,
    DLG_ATOM_STATIC = 0x0082,
    DLG_ATOM_LISTBOX = 0x0083,
    DLG_ATOM_SCROLLBAR = 0x0084,
    DLG_ATOM_COMBOBOX = 0x0085
};

struct NFC_API DialogItem {
    DialogItem();
    std::wstring className;  // 문자열 클래스. atom이면 표준 이름("BUTTON" 등)으로 채운다
    unsigned short classAtom; // atom이면 0x80~0x85, 문자열이면 0
    std::wstring text;       // 캡션 (서수 캡션이면 비어 있음)
    unsigned short textOrdinal; // 캡션이 리소스 서수(아이콘 등)면 그 값, 아니면 0
    int id;                  // 컨트롤 ID (IDC_STATIC = -1)
    unsigned long style;
    unsigned long exStyle;
    unsigned long helpId;
    short x, y, cx, cy;      // DLU
    int px, py, pw, ph;      // 픽셀 (ApplyLayout 결과)
};

struct NFC_API DialogTemplate {
    DialogTemplate();
    bool extended;           // DLGTEMPLATEEX 인가
    unsigned long style;
    unsigned long exStyle;
    short x, y, cx, cy;      // DLU
    int pw, ph;              // 픽셀 (ApplyLayout 결과)
    std::wstring title;
    std::wstring windowClass;
    bool hasFont;            // DS_SETFONT / DS_SHELLFONT
    unsigned short fontPointSize;
    unsigned short fontWeight;
    bool fontItalic;
    std::wstring fontFace;
    std::vector<DialogItem> items;

    // data/size 범위 안에서만 읽는다. 잘린 데이터면 false (그때까지 읽은 항목은 남아 있을 수 있음).
    // error가 있으면 실패 이유(영문 짧은 문장)를 넣는다.
    static bool Parse(const void* data, size_t size, DialogTemplate& out,
        std::wstring* error = NULL);
    // FindResource(RT_DIALOG) / LoadResource / LockResource 후 Parse.
    static bool LoadFromResource(HINSTANCE inst, unsigned int idd, DialogTemplate& out,
        std::wstring* error = NULL);

    // 대화상자 기본 단위(MapDialogRect 규칙): 템플릿 폰트를 dpi로 만들어
    // 평균 글자 폭(A-Z a-z 52자 / 52, 반올림)과 글자 높이를 잰다. 폰트가 없으면 시스템 폰트.
    // dpi <= 0이면 화면 DPI. 실패하면 96 DPI 기본값(6, 13)을 dpi 비율로 쓴다.
    void ComputeBaseUnits(int dpi, int& baseX, int& baseY) const;
    // DLU → 픽셀: x * baseX / 4, y * baseY / 8 (MapDialogRect와 같은 MulDiv 반올림)
    void ApplyLayout(int baseX, int baseY);
    static int DluToPixelX(int dlu, int baseX);
    static int DluToPixelY(int dlu, int baseY);
};

// 컨트롤 팩토리. item(픽셀 좌표 포함)을 보고 컨트롤을 만든다. 만들지 않으려면 빈 포인터.
// 위치/크기/ID/캡션/보이기/활성 상태는 Dialog가 팩토리 호출 뒤 공통으로 적용한다.
typedef std::function<std::shared_ptr<Control>(const DialogItem& item)> ControlFactory;

// 클래스 이름 → 팩토리 등록부. UI 스레드 전용. 이름은 대소문자 구분 없음 (윈도우 클래스 규칙).
// 내장: "NFC_Button", "NFC_Label". 표준 BUTTON(푸시버튼)/STATIC(텍스트) 매핑은 기본 켬.
class NFC_API ControlRegistry {
public:
    // 같은 이름이 있으면 바꾼다. 이름이 비었거나 factory가 비었으면 false.
    static bool Register(const wchar_t* className, ControlFactory factory);
    static bool Unregister(const wchar_t* className);
    static bool IsRegistered(const wchar_t* className);
    // 표준 BUTTON(BS_PUSHBUTTON/BS_DEFPUSHBUTTON) → Button, STATIC(SS_LEFT/CENTER/RIGHT) → Label
    static void SetMapStandardClasses(bool on);
    static bool GetMapStandardClasses();
    // item에 맞는 컨트롤을 만든다. 모르는 클래스/팩토리 실패면 빈 포인터 + 디버그 로그 한 줄.
    static std::shared_ptr<Control> CreateControl(const DialogItem& item);
};

// 짧은 이름 (ControlRegistry::Register와 같다)
NFC_API bool RegisterControlClass(const wchar_t* className, ControlFactory factory);

// 디버그 로그 한 줄 ("NFC: " 접두사, OutputDebugString). NFC_TRACE 환경 변수가 있으면 콘솔에도.
NFC_API void DebugLog(const std::wstring& line);

} // namespace nfc
