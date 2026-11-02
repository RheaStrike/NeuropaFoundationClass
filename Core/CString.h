#pragma once
// MFC/ATL CString과 API를 최대한 맞춘 STL 기반 문자열 (header-only 값 타입).
// 내부는 std::basic_string<TCHAR> (유니코드 빌드 = std::wstring). CObject를 상속하지 않는다.
// 인덱스·길이·반환은 int, 못 찾으면 -1 (MFC와 같음).
// framework.h를 먼저 포함하고 쓸 것 (TCHAR, LoadString 필요)

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cwchar>
#include <string>
#include <vector>
#include <tchar.h>

namespace nfc {

class CString {
public:
    typedef TCHAR XCHAR;
    typedef std::basic_string<TCHAR> StringType;

    CString() {}
    CString(const CString& s) : m_s(s.m_s) {}
    CString(const TCHAR* p) { if (p) m_s = p; }
    CString(const TCHAR* p, int n) { if (p && n > 0) m_s.assign(p, (size_t)n); }
    CString(TCHAR ch, int repeat = 1) { if (repeat > 0) m_s.assign((size_t)repeat, ch); }
    CString(const StringType& s) : m_s(s) {}
#ifdef _UNICODE
    // 좁은 문자열(ANSI/현재 코드 페이지)도 받는다 (MFC처럼 변환 생성)
    CString(const char* p) { if (p) m_s = Widen(p); }
    CString(const std::string& s) { m_s = Widen(s.c_str()); }
#else
    CString(const wchar_t* p) { if (p) m_s = Narrow(p); }
    CString(const std::wstring& s) { m_s = Narrow(s.c_str()); }
#endif

    CString& operator=(const CString& s) { m_s = s.m_s; return *this; }
    CString& operator=(const TCHAR* p) { if (p) m_s = p; else m_s.clear(); return *this; }
    CString& operator=(TCHAR ch) { m_s.assign(1, ch); return *this; }
    CString& operator=(const StringType& s) { m_s = s; return *this; }

    CString& operator+=(const CString& s) { m_s += s.m_s; return *this; }
    CString& operator+=(const TCHAR* p) { if (p) m_s += p; return *this; }
    CString& operator+=(TCHAR ch) { m_s += ch; return *this; }

    // --- 변환 ---
    operator const TCHAR*() const { return m_s.c_str(); }
    const TCHAR* GetString() const { return m_s.c_str(); }
    const TCHAR* c_str() const { return m_s.c_str(); }
    const StringType& str() const { return m_s; }
    std::wstring ToWString() const {
#ifdef _UNICODE
        return m_s;
#else
        return Widen(m_s.c_str());
#endif
    }
    std::string ToString() const { // 현재 코드 페이지(ANSI)
#ifdef _UNICODE
        return Narrow(m_s.c_str());
#else
        return m_s;
#endif
    }
    std::string ToUtf8() const {
        std::wstring w = ToWString();
        return Convert(w.c_str(), CP_UTF8);
    }

    // --- 길이·원소 ---
    int GetLength() const { return (int)m_s.size(); }
    bool IsEmpty() const { return m_s.empty(); }
    void Empty() { m_s.clear(); }
    TCHAR GetAt(int i) const { return m_s.at((size_t)i); }
    void SetAt(int i, TCHAR ch) { m_s.at((size_t)i) = ch; }
    TCHAR operator[](int i) const { return m_s.at((size_t)i); }

    // --- 버퍼 ---
    // GetBuffer(n): 최소 n 글자 쓸 수 있는 버퍼. 쓴 뒤 ReleaseBuffer(길이 or -1=NUL까지).
    TCHAR* GetBuffer(int minLen = 0) {
        if (minLen > (int)m_s.size()) m_s.resize((size_t)minLen);
        return &m_s[0];
    }
    TCHAR* GetBufferSetLength(int len) {
        m_s.resize(len > 0 ? (size_t)len : 0);
        return &m_s[0];
    }
    void ReleaseBuffer(int newLen = -1) {
        if (newLen < 0) newLen = (int)std::char_traits<TCHAR>::length(m_s.c_str());
        if (newLen > (int)m_s.size()) newLen = (int)m_s.size();
        m_s.resize((size_t)newLen);
    }
    void FreeExtra() { m_s.shrink_to_fit(); }
    void Preallocate(int n) { if (n > 0) m_s.reserve((size_t)n); }
    void Truncate(int len) { if (len >= 0 && len < (int)m_s.size()) m_s.resize((size_t)len); }

    // --- 비교 ---
    int Compare(const TCHAR* p) const { return Sign(_tcscmp(m_s.c_str(), p ? p : _T(""))); }
    int CompareNoCase(const TCHAR* p) const { return Sign(_tcsicmp(m_s.c_str(), p ? p : _T(""))); }
    int Collate(const TCHAR* p) const { return Sign(_tcscoll(m_s.c_str(), p ? p : _T(""))); }
    int CollateNoCase(const TCHAR* p) const { return Sign(_tcsicoll(m_s.c_str(), p ? p : _T(""))); }

    // --- 부분 문자열 ---
    CString Left(int n) const {
        if (n <= 0) return CString();
        return CString(m_s.substr(0, (size_t)n));
    }
    CString Right(int n) const {
        if (n <= 0) return CString();
        int len = GetLength();
        if (n >= len) return *this;
        return CString(m_s.substr((size_t)(len - n)));
    }
    CString Mid(int first) const { return Mid(first, GetLength()); }
    CString Mid(int first, int count) const {
        int len = GetLength();
        if (first < 0) first = 0;
        if (count < 0) count = 0;
        if (first >= len) return CString();
        if (count > len - first) count = len - first;
        return CString(m_s.substr((size_t)first, (size_t)count));
    }
    CString SpanIncluding(const TCHAR* set) const {
        if (!set) return CString();
        size_t p = m_s.find_first_not_of(set);
        return p == StringType::npos ? *this : CString(m_s.substr(0, p));
    }
    CString SpanExcluding(const TCHAR* set) const {
        if (!set) return *this;
        size_t p = m_s.find_first_of(set);
        return p == StringType::npos ? *this : CString(m_s.substr(0, p));
    }

    // --- 검색 (실패 -1) ---
    int Find(TCHAR ch, int start = 0) const {
        if (start < 0 || start >= GetLength()) return -1;
        return Pos(m_s.find(ch, (size_t)start));
    }
    int Find(const TCHAR* sub, int start = 0) const {
        if (!sub || start < 0 || start > GetLength()) return -1;
        if (!*sub) return -1; // MFC: 빈 문자열은 못 찾음 취급
        return Pos(m_s.find(sub, (size_t)start));
    }
    int FindOneOf(const TCHAR* set) const {
        if (!set || !*set) return -1;
        return Pos(m_s.find_first_of(set));
    }
    int ReverseFind(TCHAR ch) const { return Pos(m_s.rfind(ch)); }

    // --- 수정 ---
    int Replace(TCHAR oldCh, TCHAR newCh) {
        int n = 0;
        if (oldCh == newCh) return 0;
        for (size_t i = 0; i < m_s.size(); ++i) if (m_s[i] == oldCh) { m_s[i] = newCh; ++n; }
        return n;
    }
    int Replace(const TCHAR* oldS, const TCHAR* newS) {
        if (!oldS || !*oldS) return 0;
        StringType o(oldS), r(newS ? newS : _T(""));
        int n = 0;
        size_t p = 0;
        while ((p = m_s.find(o, p)) != StringType::npos) {
            m_s.replace(p, o.size(), r);
            p += r.size();
            ++n;
        }
        return n;
    }
    int Remove(TCHAR ch) {
        size_t before = m_s.size();
        m_s.erase(std::remove(m_s.begin(), m_s.end(), ch), m_s.end());
        return (int)(before - m_s.size());
    }
    int Insert(int index, TCHAR ch) {
        if (index < 0) index = 0;
        if (index > GetLength()) index = GetLength();
        m_s.insert((size_t)index, 1, ch);
        return GetLength();
    }
    int Insert(int index, const TCHAR* p) {
        if (index < 0) index = 0;
        if (index > GetLength()) index = GetLength();
        if (p) m_s.insert((size_t)index, p);
        return GetLength();
    }
    int Delete(int index, int count = 1) {
        if (index < 0) index = 0;
        if (count > 0 && index < GetLength()) {
            if (count > GetLength() - index) count = GetLength() - index;
            m_s.erase((size_t)index, (size_t)count);
        }
        return GetLength();
    }

    CString& TrimLeft() { return TrimLeft(_T(" \t\r\n\v\f")); }
    CString& TrimLeft(TCHAR ch) { TCHAR s[2] = { ch, 0 }; return TrimLeft(s); }
    CString& TrimLeft(const TCHAR* set) {
        if (!set) return *this;
        size_t p = m_s.find_first_not_of(set);
        if (p == StringType::npos) m_s.clear(); else m_s.erase(0, p);
        return *this;
    }
    CString& TrimRight() { return TrimRight(_T(" \t\r\n\v\f")); }
    CString& TrimRight(TCHAR ch) { TCHAR s[2] = { ch, 0 }; return TrimRight(s); }
    CString& TrimRight(const TCHAR* set) {
        if (!set) return *this;
        size_t p = m_s.find_last_not_of(set);
        if (p == StringType::npos) m_s.clear(); else m_s.erase(p + 1);
        return *this;
    }
    CString& Trim() { TrimRight(); return TrimLeft(); }
    CString& Trim(TCHAR ch) { TrimRight(ch); return TrimLeft(ch); }
    CString& Trim(const TCHAR* set) { TrimRight(set); return TrimLeft(set); }

    CString& MakeUpper() { if (!m_s.empty()) _tcsupr_s(&m_s[0], m_s.size() + 1); return *this; }
    CString& MakeLower() { if (!m_s.empty()) _tcslwr_s(&m_s[0], m_s.size() + 1); return *this; }
    CString& MakeReverse() { std::reverse(m_s.begin(), m_s.end()); return *this; }

    // pos부터 delims로 끊은 다음 토큰. 다 쓰면 빈 문자열 + pos = -1 (MFC와 같음)
    CString Tokenize(const TCHAR* delims, int& pos) const {
        if (pos < 0 || pos >= GetLength() || !delims) { pos = -1; return CString(); }
        size_t b = m_s.find_first_not_of(delims, (size_t)pos);
        if (b == StringType::npos) { pos = -1; return CString(); }
        size_t e = m_s.find_first_of(delims, b);
        if (e == StringType::npos) e = m_s.size();
        pos = (int)e + 1;
        return CString(m_s.substr(b, e - b));
    }

    CString& Append(const CString& s) { m_s += s.m_s; return *this; }
    CString& Append(const TCHAR* p) { if (p) m_s += p; return *this; }
    CString& Append(const TCHAR* p, int n) { if (p && n > 0) m_s.append(p, (size_t)n); return *this; }
    CString& AppendChar(TCHAR ch) { m_s += ch; return *this; }

    // --- 서식 (printf 계열). 필요한 길이를 먼저 구해 할당하므로 넘치지 않는다 ---
    void Format(const TCHAR* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        FormatV(fmt, ap);
        va_end(ap);
    }
    void FormatV(const TCHAR* fmt, va_list ap) { m_s = Printf(fmt, ap); }
    void AppendFormat(const TCHAR* fmt, ...) {
        va_list ap;
        va_start(ap, fmt);
        m_s += Printf(fmt, ap);
        va_end(ap);
    }

    // 문자열 리소스 읽기. 실패하면 false (내용은 비움)
    BOOL LoadString(HINSTANCE inst, UINT id) {
        const TCHAR* p = NULL;
        int n = ::LoadString(inst, id, reinterpret_cast<TCHAR*>(&p), 0); // 읽기 전용 포인터
        if (n <= 0 || !p) { m_s.clear(); return FALSE; }
        m_s.assign(p, (size_t)n);
        return TRUE;
    }
    BOOL LoadString(UINT id) { return LoadString(GetModuleHandle(NULL), id); }

    // --- 수 변환 보조 (MFC에는 없음) ---
    int ToInt(int base = 10) const { return (int)_tcstol(m_s.c_str(), NULL, base); }
    double ToDouble() const { return _tcstod(m_s.c_str(), NULL); }
    static CString FromInt(int v) { CString s; s.Format(_T("%d"), v); return s; }
    static CString FromDouble(double v) { CString s; s.Format(_T("%g"), v); return s; }

private:
    static int Sign(int v) { return v < 0 ? -1 : (v > 0 ? 1 : 0); }
    static int Pos(size_t p) { return p == StringType::npos ? -1 : (int)p; }
    static StringType Printf(const TCHAR* fmt, va_list ap) {
        if (!fmt) return StringType();
        va_list cp;
        va_copy(cp, ap);
        int n = _vsctprintf(fmt, cp); // 필요한 길이 조회
        va_end(cp);
        if (n <= 0) return StringType();
        std::vector<TCHAR> buf((size_t)n + 1);
        _vstprintf_s(&buf[0], buf.size(), fmt, ap);
        return StringType(&buf[0], (size_t)n);
    }
    static std::string Convert(const wchar_t* w, UINT cp) {
        int n = WideCharToMultiByte(cp, 0, w, -1, NULL, 0, NULL, NULL);
        if (n <= 1) return std::string();
        std::string s((size_t)n, '\0');
        WideCharToMultiByte(cp, 0, w, -1, &s[0], n, NULL, NULL);
        s.resize((size_t)n - 1);
        return s;
    }
    static std::string Narrow(const wchar_t* w) { return Convert(w, CP_ACP); }
    static std::wstring Widen(const char* a) {
        int n = MultiByteToWideChar(CP_ACP, 0, a, -1, NULL, 0);
        if (n <= 1) return std::wstring();
        std::wstring s((size_t)n, L'\0');
        MultiByteToWideChar(CP_ACP, 0, a, -1, &s[0], n);
        s.resize((size_t)n - 1);
        return s;
    }

    StringType m_s;
};

// --- 비교·연결 (CString/LPCTSTR 양쪽) ---
inline bool operator==(const CString& a, const CString& b) { return a.str() == b.str(); }
inline bool operator==(const CString& a, const TCHAR* b) { return a.Compare(b) == 0; }
inline bool operator==(const TCHAR* a, const CString& b) { return b.Compare(a) == 0; }
inline bool operator!=(const CString& a, const CString& b) { return !(a == b); }
inline bool operator!=(const CString& a, const TCHAR* b) { return !(a == b); }
inline bool operator!=(const TCHAR* a, const CString& b) { return !(a == b); }
inline bool operator<(const CString& a, const CString& b) { return a.str() < b.str(); }
inline bool operator>(const CString& a, const CString& b) { return b < a; }
inline bool operator<=(const CString& a, const CString& b) { return !(b < a); }
inline bool operator>=(const CString& a, const CString& b) { return !(a < b); }
inline bool operator<(const CString& a, const TCHAR* b) { return a.Compare(b) < 0; }
inline bool operator>(const CString& a, const TCHAR* b) { return a.Compare(b) > 0; }
inline bool operator<=(const CString& a, const TCHAR* b) { return a.Compare(b) <= 0; }
inline bool operator>=(const CString& a, const TCHAR* b) { return a.Compare(b) >= 0; }

inline CString operator+(const CString& a, const CString& b) { CString r(a); r += b; return r; }
inline CString operator+(const CString& a, const TCHAR* b) { CString r(a); r += b; return r; }
inline CString operator+(const TCHAR* a, const CString& b) { CString r(a); r += b; return r; }
inline CString operator+(const CString& a, TCHAR b) { CString r(a); r += b; return r; }
inline CString operator+(TCHAR a, const CString& b) { CString r(a); r += b; return r; }

} // namespace nfc
