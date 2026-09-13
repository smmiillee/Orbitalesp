// --- src/overlay.h ---
#pragma once
#include <d3d11.h>
#include <string>

class Overlay {
public:
    HWND                    hwnd      = nullptr;
    ID3D11Device*           device    = nullptr;
    ID3D11DeviceContext*    context   = nullptr;
    IDXGISwapChain*         swapchain = nullptr;
    ID3D11RenderTargetView* rtv       = nullptr;

    bool create(int width, int height);
    void begin_frame();   // calls update_input_mode() internally
    void end_frame();
    void cleanup();
    void update_input_mode(); // toggle click-through based on g_menu_open

    bool is_running() const { return hwnd != nullptr; }

private:
    bool create_device();
    bool create_render_target();
    void release_render_target();

    static LRESULT CALLBACK wnd_proc(HWND, UINT, WPARAM, LPARAM);
    WNDCLASSEXW wc{};
};
