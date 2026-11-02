#pragma once
#include "framework.h"

namespace demo {

// 앱 수명(App::Init/Shutdown)과 데모 실행을 묶는다. wWinMain/wmain은 이것만 부른다.
class DemoApp {
public:
    // GUI: 메인 데모 창을 모달로 띄우고 결과를 반환
    // idd = true면 메인 창 대신 IDD 리소스 폼(IDD_DEMO_FORM)을 바로 띄운다 (--idd)
    static int RunGui(HINSTANCE h, bool idd = false);
    // CUI: 창 없이 워커 예제만 (Wnd::Create는 CUI에서 assert + 실패)
    static int RunConsole();
};

} // namespace demo
