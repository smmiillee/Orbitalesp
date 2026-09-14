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
    void sync_to_game();

    static void enable_dpi_awareness();

private:
    bool init_dx11(int width, int height);
    bool create_rtv();
    void release_rtv();
    bool resize_buffers(int width, int height);
    void update_visibility_and_input();

    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    WNDCLASSEXW wc{};

    int width_  = 0;
    int height_ = 0;
};
