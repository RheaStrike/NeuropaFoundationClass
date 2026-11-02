// NFC_Demo 진입점. 실제 내용은 DemoApp(데모 창)과 SelfTest(회귀 테스트)에 있다.
#include "framework.h"
#include "DemoApp.h"
#include "SelfTest.h"

#ifdef NFC_CUI
int wmain(int argc, wchar_t* argv[]) {
    if (argc > 1 && lstrcmpW(argv[1], L"--selftest") == 0)
        return SelfTest::Run();
    return demo::DemoApp::RunConsole();
}
#else
int WINAPI wWinMain(HINSTANCE h, HINSTANCE, LPWSTR cmd, int) {
    // --idd: IDD 리소스 폼(IDD_DEMO_FORM)을 바로 띄운다
    bool idd = cmd && wcsstr(cmd, L"--idd") != NULL;
    return demo::DemoApp::RunGui(h, idd);
}
#endif
