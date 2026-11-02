#include "DemoApp.h"
#include "App.h"
#include "DemoDialogs.h"
#include <cstdio>
#include <exception>
#include <future>

namespace demo {

int DemoApp::RunGui(HINSTANCE h, bool idd) {
    try {
        if (!nfc::App::Init(h)) {
            MessageBoxA(NULL, "App::Init failed", "NFC_Demo", MB_ICONERROR);
            return 1;
        }
        int r = 0;
        {
            // dlg는 이 블록 안에서 죽는다 (App::Shutdown 전에 창을 닫는다).
            if (idd) {
                DemoFormDialog form;
                r = form.Run(NULL);
            } else {
                DemoDialog dlg;
                r = dlg.Run();
            }
        }
        nfc::App::Shutdown();
        return r;
    } catch (const std::exception& e) {
        MessageBoxA(NULL, e.what(), "NFC_Demo", MB_ICONERROR);
        return 1;
    }
}

int DemoApp::RunConsole() {
    wprintf(L"NFC_Demo (CUI)\n");
    if (!nfc::App::Init(GetModuleHandle(NULL))) {
        wprintf(L"init failed\n");
        return 1;
    }
    std::future<int> fut = nfc::App::RunWorker([]() { return 40 + 2; });
    wprintf(L"worker=%d\n", fut.get());
    nfc::App::Shutdown();
    return 0;
}

} // namespace demo
