// --- src/overlay.h ---
#pragma once
#include <d3d11.h>

class Overlay {
public:
    HWND                    hwnd      = nullptr;
    ID3D11Device*           device    = nullptr;
    ID3D11DeviceContext*    context   = nullptr;
    IDXGISwapChain*         swapchain = nullptr;
    ID3D11RenderTargetView* rtv       = nullptr;

    bool create(int width, int height);
    void begin_frame();
    void end_frame();
    void cleanup();

private:
    bool init_dx11(int width, int height);
    bool create_rtv();
    void release_rtv();
    void update_visibility_and_input();

    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    WNDCLASSEXW wc{};
};
