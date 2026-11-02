#include "framework.h"
#include "DialogTemplate.h"
#include "Control.h"
#include "Button.h"
#include "Label.h"
#include <map>

namespace nfc {

// ===================== 디버그 로그 =====================
void DebugLog(const std::wstring& line) {
    std::wstring s = L"NFC: " + line + L"\n";
    OutputDebugStringW(s.c_str());
    if (GetEnvironmentVariableW(L"NFC_TRACE", NULL, 0)) {
        wprintf(L"%ls", s.c_str());
        fflush(stdout);
    }
}

DialogItem::DialogItem()
    : classAtom(0), textOrdinal(0), id(0), style(0), exStyle(0), helpId(0),
      x(0), y(0), cx(0), cy(0), px(0), py(0), pw(0), ph(0) {
}

DialogTemplate::DialogTemplate()
    : extended(false), style(0), exStyle(0), x(0), y(0), cx(0), cy(0), pw(0), ph(0),
      hasFont(false), fontPointSize(0), fontWeight(0), fontItalic(false) {
}

// ===================== 파서 =====================
namespace {

// 범위 검사가 붙은 읽기 커서. 모든 읽기는 [base, base+size) 안에서만 한다.
class Reader {
public:
    Reader(const void* data, size_t size)
        : m_base((const unsigned char*)data), m_size(data ? size : 0), m_pos(0), m_ok(true) {}
    bool Ok() const { return m_ok; }
    size_t Pos() const { return m_pos; }

    bool Word(unsigned short& v) {
        if (!Need(2)) return false;
        v = (unsigned short)(m_base[m_pos] | (m_base[m_pos + 1] << 8));
        m_pos += 2;
        return true;
    }
    bool Short(short& v) {
        unsigned short w = 0;
        if (!Word(w)) return false;
        v = (short)w;
        return true;
    }
    bool DWord(unsigned long& v) {
        if (!Need(4)) return false;
        v = (unsigned long)m_base[m_pos] | ((unsigned long)m_base[m_pos + 1] << 8) |
            ((unsigned long)m_base[m_pos + 2] << 16) | ((unsigned long)m_base[m_pos + 3] << 24);
        m_pos += 4;
        return true;
    }
    bool Byte(unsigned char& v) {
        if (!Need(1)) return false;
        v = m_base[m_pos++];
        return true;
    }
    bool Skip(size_t n) {
        if (!Need(n)) return false;
        m_pos += n;
        return true;
    }
    // 0으로 끝나는 UTF-16 문자열. 끝(0)이 범위 밖이면 실패.
    bool String(std::wstring& s) {
        s.clear();
        for (;;) {
            unsigned short c = 0;
            if (!Word(c)) return false;
            if (c == 0) return true;
            s.push_back((wchar_t)c);
        }
    }
    // sz_Or_Ord: 0x0000 없음 / 0xFFFF + 서수 / 문자열
    bool SzOrOrd(std::wstring& s, unsigned short& ord) {
        s.clear();
        ord = 0;
        if (!Need(2)) return false;
        unsigned short first = (unsigned short)(m_base[m_pos] | (m_base[m_pos + 1] << 8));
        if (first == 0x0000) { m_pos += 2; return true; }
        if (first == 0xFFFF) {
            m_pos += 2;
            return Word(ord);
        }
        return String(s);
    }
    // 템플릿 시작 기준 DWORD 정렬 (항목은 DWORD 경계에서 시작)
    bool AlignDWord() {
        size_t pad = (4 - (m_pos & 3)) & 3;
        if (pad == 0) return true;
        return Skip(pad);
    }

private:
    bool Need(size_t n) {
        if (!m_ok || n > m_size || m_pos > m_size - n) { m_ok = false; return false; }
        return true;
    }
    const unsigned char* m_base;
    size_t m_size;
    size_t m_pos;
    bool m_ok;
};

const wchar_t* AtomName(unsigned short atom) {
    switch (atom) {
    case DLG_ATOM_BUTTON: return L"BUTTON";
    case DLG_ATOM_EDIT: return L"EDIT";
    case DLG_ATOM_STATIC: return L"STATIC";
    case DLG_ATOM_LISTBOX: return L"LISTBOX";
    case DLG_ATOM_SCROLLBAR: return L"SCROLLBAR";
    case DLG_ATOM_COMBOBOX: return L"COMBOBOX";
    default: return NULL;
    }
}

bool SetError(std::wstring* error, const wchar_t* what) {
    if (error) *error = what;
    return false;
}

} // namespace

bool DialogTemplate::Parse(const void* data, size_t size, DialogTemplate& out,
        std::wstring* error) {
    out = DialogTemplate();
    if (error) error->clear();
    if (!data || size == 0) return SetError(error, L"empty template");
    Reader r(data, size);
    unsigned short cdit = 0;

    // --- 머리 ---
    unsigned short ver = 0, sig = 0;
    if (!r.Word(ver) || !r.Word(sig)) return SetError(error, L"truncated header");
    if (ver == 1 && sig == 0xFFFF) {
        out.extended = true;
        unsigned long help = 0;
        if (!r.DWord(help) || !r.DWord(out.exStyle) || !r.DWord(out.style))
            return SetError(error, L"truncated header");
    } else {
        // DLGTEMPLATE: 첫 DWORD = style (방금 읽은 두 WORD)
        out.extended = false;
        out.style = (unsigned long)ver | ((unsigned long)sig << 16);
        if (!r.DWord(out.exStyle)) return SetError(error, L"truncated header");
    }
    if (!r.Word(cdit) || !r.Short(out.x) || !r.Short(out.y) ||
        !r.Short(out.cx) || !r.Short(out.cy))
        return SetError(error, L"truncated header");
    std::wstring menu;
    unsigned short ord = 0;
    if (!r.SzOrOrd(menu, ord)) return SetError(error, L"truncated menu");
    if (!r.SzOrOrd(out.windowClass, ord)) return SetError(error, L"truncated class");
    if (!r.String(out.title)) return SetError(error, L"truncated title");
    // DS_SHELLFONT = DS_SETFONT | DS_FIXEDSYS 이므로 DS_SETFONT 비트로 판정
    if (out.style & DS_SETFONT) {
        out.hasFont = true;
        if (!r.Word(out.fontPointSize)) return SetError(error, L"truncated font");
        if (out.extended) {
            unsigned char italic = 0, charset = 0;
            if (!r.Word(out.fontWeight) || !r.Byte(italic) || !r.Byte(charset))
                return SetError(error, L"truncated font");
            out.fontItalic = italic != 0;
        }
        if (!r.String(out.fontFace)) return SetError(error, L"truncated font face");
    }

    // --- 항목 ---
    out.items.reserve(cdit < 256 ? cdit : 256); // 잘못된 개수로 큰 할당을 하지 않는다
    for (unsigned short i = 0; i < cdit; ++i) {
        DialogItem it;
        if (!r.AlignDWord()) return SetError(error, L"truncated item");
        if (out.extended) {
            unsigned long id = 0;
            if (!r.DWord(it.helpId) || !r.DWord(it.exStyle) || !r.DWord(it.style) ||
                !r.Short(it.x) || !r.Short(it.y) || !r.Short(it.cx) || !r.Short(it.cy) ||
                !r.DWord(id))
                return SetError(error, L"truncated item");
            it.id = (int)id;
        } else {
            unsigned short id = 0;
            if (!r.DWord(it.style) || !r.DWord(it.exStyle) ||
                !r.Short(it.x) || !r.Short(it.y) || !r.Short(it.cx) || !r.Short(it.cy) ||
                !r.Word(id))
                return SetError(error, L"truncated item");
            it.id = (id == 0xFFFF) ? -1 : (int)id; // IDC_STATIC
        }
        if (!r.SzOrOrd(it.className, it.classAtom)) return SetError(error, L"truncated item class");
        if (it.classAtom != 0) {
            const wchar_t* n = AtomName(it.classAtom);
            if (n) it.className = n;
        }
        if (!r.SzOrOrd(it.text, it.textOrdinal)) return SetError(error, L"truncated item text");
        unsigned short extra = 0;
        if (!r.Word(extra)) return SetError(error, L"truncated item extra");
        if (!r.Skip(extra)) return SetError(error, L"truncated item data");
        out.items.push_back(it);
    }
    return true;
}

bool DialogTemplate::LoadFromResource(HINSTANCE inst, unsigned int idd, DialogTemplate& out,
        std::wstring* error) {
    out = DialogTemplate();
    HRSRC res = FindResourceW(inst, MAKEINTRESOURCEW(idd), MAKEINTRESOURCEW(5)); // RT_DIALOG
    if (!res) return SetError(error, L"dialog resource not found");
    DWORD size = SizeofResource(inst, res);
    HGLOBAL h = ::LoadResource(inst, res);
    const void* p = h ? LockResource(h) : NULL;
    if (!p || size == 0) return SetError(error, L"dialog resource load failed");
    return Parse(p, size, out, error);
}

int DialogTemplate::DluToPixelX(int dlu, int baseX) { return MulDiv(dlu, baseX, 4); }
int DialogTemplate::DluToPixelY(int dlu, int baseY) { return MulDiv(dlu, baseY, 8); }

void DialogTemplate::ComputeBaseUnits(int dpi, int& baseX, int& baseY) const {
    HDC dc = GetDC(NULL);
    if (dpi <= 0) dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dpi <= 0) dpi = 96;
    baseX = MulDiv(6, dpi, 96);
    baseY = MulDiv(13, dpi, 96);
    if (!dc) return;
    HFONT font = NULL;
    if (hasFont && !fontFace.empty()) {
        // "MS Shell Dlg"는 글꼴 대체로 실제 글꼴이 정해진다.
        font = CreateFontW(-MulDiv(fontPointSize ? fontPointSize : 8, dpi, 72), 0, 0, 0,
            fontWeight ? fontWeight : FW_NORMAL, fontItalic ? TRUE : FALSE, FALSE, FALSE,
            DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, DEFAULT_QUALITY,
            DEFAULT_PITCH | FF_DONTCARE, fontFace.c_str());
    }
    HGDIOBJ old = SelectObject(dc, font ? (HGDIOBJ)font : GetStockObject(SYSTEM_FONT));
    TEXTMETRICW tm;
    SIZE sz = { 0, 0 };
    static const wchar_t kAlpha[] = L"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";
    if (GetTextMetricsW(dc, &tm) && GetTextExtentPoint32W(dc, kAlpha, 52, &sz) && sz.cx > 0) {
        baseX = (sz.cx / 26 + 1) / 2; // MapDialogRect와 같은 반올림
        baseY = tm.tmHeight;
        if (!font) { // 시스템 폰트는 96 DPI 기준값이므로 DPI 비율을 곱한다
            int sysDpi = GetDeviceCaps(dc, LOGPIXELSY);
            if (sysDpi > 0 && sysDpi != dpi) {
                baseX = MulDiv(baseX, dpi, sysDpi);
                baseY = MulDiv(baseY, dpi, sysDpi);
            }
        }
    }
    SelectObject(dc, old);
    if (font) DeleteObject(font);
    ReleaseDC(NULL, dc);
    if (baseX <= 0) baseX = 1;
    if (baseY <= 0) baseY = 1;
}

void DialogTemplate::ApplyLayout(int baseX, int baseY) {
    pw = DluToPixelX(cx, baseX);
    ph = DluToPixelY(cy, baseY);
    for (size_t i = 0; i < items.size(); ++i) {
        DialogItem& it = items[i];
        it.px = DluToPixelX(it.x, baseX);
        it.py = DluToPixelY(it.y, baseY);
        it.pw = DluToPixelX(it.cx, baseX);
        it.ph = DluToPixelY(it.cy, baseY);
    }
}

// ===================== 등록부 =====================
namespace {

std::wstring Upper(const wchar_t* s) {
    std::wstring u(s ? s : L"");
    for (size_t i = 0; i < u.size(); ++i) {
        if (u[i] >= L'a' && u[i] <= L'z') u[i] = (wchar_t)(u[i] - L'a' + L'A');
    }
    return u;
}

std::shared_ptr<Control> MakeButton(const DialogItem&) {
    return std::shared_ptr<Control>(new Button());
}

std::shared_ptr<Control> MakeLabel(const DialogItem& item) {
    std::shared_ptr<Label> l(new Label());
    // STATIC 정렬 스타일을 그대로 따른다 (NFC_Label도 같은 스타일 비트를 쓸 수 있다)
    unsigned long t = item.style & SS_TYPEMASK;
    if (t == SS_CENTER) l->SetAlign(1);
    else if (t == SS_RIGHT) l->SetAlign(2);
    return l;
}

struct Registry {
    std::map<std::wstring, ControlFactory> factories; // 키 = 대문자 이름
    bool mapStandard;
    Registry() : mapStandard(true) {
        factories[L"NFC_BUTTON"] = MakeButton;
        factories[L"NFC_LABEL"] = MakeLabel;
    }
};

Registry& Reg() {
    static Registry r; // DLL 안에 하나 (헤더 인라인 static 금지 규칙)
    return r;
}

} // namespace

bool ControlRegistry::Register(const wchar_t* className, ControlFactory factory) {
    if (!className || !*className || !factory) return false;
    Reg().factories[Upper(className)] = factory;
    return true;
}

bool ControlRegistry::Unregister(const wchar_t* className) {
    return Reg().factories.erase(Upper(className)) > 0;
}

bool ControlRegistry::IsRegistered(const wchar_t* className) {
    return Reg().factories.find(Upper(className)) != Reg().factories.end();
}

void ControlRegistry::SetMapStandardClasses(bool on) { Reg().mapStandard = on; }
bool ControlRegistry::GetMapStandardClasses() { return Reg().mapStandard; }

std::shared_ptr<Control> ControlRegistry::CreateControl(const DialogItem& item) {
    Registry& reg = Reg();
    ControlFactory f;
    if (item.classAtom == 0) {
        std::map<std::wstring, ControlFactory>::const_iterator i =
            reg.factories.find(Upper(item.className.c_str()));
        if (i != reg.factories.end()) f = i->second;
    } else if (reg.mapStandard) {
        if (item.classAtom == DLG_ATOM_BUTTON) {
            unsigned long t = item.style & BS_TYPEMASK;
            if (t == BS_PUSHBUTTON || t == BS_DEFPUSHBUTTON) f = MakeButton;
        } else if (item.classAtom == DLG_ATOM_STATIC) {
            unsigned long t = item.style & SS_TYPEMASK;
            if (t == SS_LEFT || t == SS_CENTER || t == SS_RIGHT) f = MakeLabel;
        }
    }
    if (!f) {
        DebugLog(L"dialog item skipped: unknown class '" + item.className +
            L"' id=" + std::to_wstring(item.id));
        return std::shared_ptr<Control>();
    }
    std::shared_ptr<Control> c;
    try {
        c = f(item);
    } catch (...) {
        DebugLog(L"dialog item skipped: factory threw for class '" + item.className + L"'");
        return std::shared_ptr<Control>();
    }
    if (!c) DebugLog(L"dialog item skipped: factory returned null for '" + item.className + L"'");
    return c;
}

bool RegisterControlClass(const wchar_t* className, ControlFactory factory) {
    return ControlRegistry::Register(className, factory);
}

} // namespace nfc
