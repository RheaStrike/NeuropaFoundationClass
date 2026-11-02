#pragma once

// DLL로 내보내는 클래스에 STL/GDI+ 멤버가 있으면 뜨는 상용 경고 끄기.
// 같은 컴파일러·같은 CRT로만 붙는다는 전제 (프레임워크 표준 방식).
#ifdef _MSC_VER
#pragma warning(disable: 4251)
#pragma warning(disable: 4275)
#endif

#if defined(NFC_STATIC_LIB)
#define NFC_API
#elif defined(NFC_CORE_EXPORTS)
#define NFC_API __declspec(dllexport)
#else
#define NFC_API __declspec(dllimport)
#endif
