#pragma once
#include "CObject.h"
#include "Renderer.h"
#include <memory>
#include <string>

// framework.h를 먼저 포함하고 쓸 것 (HWND 등 필요)

namespace nfc {

// HWND 래퍼. 기본값 = 레이어드 보더리스 창 (WS_EX_LAYERED, WS_POPUP).
// UI 스레드 전용. 다른 스레드에서 건드리면 assert. 워커 → UI는 App::PostToUI.
// 그리기는 Renderer(지금은 GDI+)가 맡는다. 표면은 크기가 바뀔 때만 다시 만든다.
class NFC_API Wnd : public CObject {
public:
    Wnd();
    // 파생 클래스는 자기 소멸자에서 Close()를 먼저 불러야 파생 OnDestroy가 불린다.
    // 여기까지 창이 남아 있으면 콜백 없이(분리 후) 조용히 파괴한다.
    virtual ~Wnd();

    bool Create(Wnd* parent, const wchar_t* title, int x, int y, int w, int h,
        DWORD style = WS_POPUP,
        DWORD exStyle = WS_EX_LAYERED | WS_EX_APPWINDOW);

    HWND GetHwnd() const { return m_hwnd; }
    int GetWidth() const { return m_w; }
    int GetHeight() const { return m_h; }

    void Show(int cmd = SW_SHOW);
    void Hide();
    void Close();
    // 다시 그리기 예약. 여러 번 불러도 메시지 한 번에 한 번만 그린다 (합치기).
    void Invalidate();
    void Move(int x, int y);
    void Resize(int w, int h);
    void CenterWindow();
    void SetText(const std::wstring& t);

    static LRESULT CALLBACK WndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp); // 내부용
    static Wnd* FromHwnd(HWND h);

protected:
    virtual LRESULT HandleMessage(UINT msg, WPARAM wp, LPARAM lp);
    virtual void OnPaintLayered(Renderer& r, int w, int h);
    virtual void OnCreate();
    virtual void OnDestroy();
    virtual void OnSize(int w, int h);
    virtual bool OnLeftDown(int x, int y); // true면 처리됨(마우스 캡처), false면 창 드래그
    virtual void OnLeftUp(int x, int y);
    virtual void OnMouseMove(int x, int y);
    virtual void OnMouseLeave();
    virtual void OnCaptureLost();
    virtual void OnKeyDown(unsigned vk);
    virtual void OnChar(wchar_t ch);

    void RenderLayered(); // 지금 바로 그린다

    HWND m_hwnd;
    int m_w, m_h;

private:
    static bool EnsureClass(HINSTANCE h);

    std::unique_ptr<Renderer> m_renderer;
    bool m_renderPending; // 그리기 메시지가 이미 큐에 있음
    bool m_trackingMouse; // TrackMouseEvent(TME_LEAVE) 등록됨
};

} // namespace nfc
