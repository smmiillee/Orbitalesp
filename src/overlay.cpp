// --- src/overlay.cpp ---
//
// DWM per-pixel alpha overlay — topmost layered popup.
//
// Why not WS_CHILD parented to CS2:
//   CreateWindowExW with a cross-process parent HWND silently returns NULL
//   on Windows Vista+. The DX11 init then crashes on OutputWindow=nullptr.
//
// Correct approach:
//   1. WS_POPUP | WS_EX_TOPMOST | WS_EX_LAYERED — our own top-level window.
//   2. SetLayeredWindowAttributes LWA_ALPHA 255 — Win32 treats the window as
//      fully opaque. Actual transparency comes from DX11 alpha + DWM.
//   3. DwmExtendFrameIntoClientArea MARGINS{-1} — extends the DWM glass frame
//      over the entire client area. This activates per-pixel alpha compositing
//      so DX11 clear color alpha=0.0 is truly transparent, not black.
//   4. Visibility tied to CS2 foreground state: we call ShowWindow(SW_HIDE)
//      the moment CS2 is not the foreground window, SW_SHOW when it is.
//      Checked once per frame in begin_frame — zero overhead.
//
#include "overlay.h"
#include <dwmapi.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
extern bool g_menu_open;  // defined in main.cpp
extern HWND g_cs2_hwnd;   // defined in main.cpp — used to check foreground

LRESULT CALLBACK Overlay::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool Overlay::create(int width, int height) {
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"OrbitalESP_Overlay";
    if (!RegisterClassExW(&wc)) return false;

    hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        wc.lpszClassName, L"OrbitalESP",
        WS_POPUP,
        0, 0, width, height,
        nullptr, nullptr, wc.hInstance, nullptr
    );
    if (!hwnd) return false;

    // LWA_ALPHA 255: fully opaque at Win32 level.
    // Per-pixel transparency comes from DX11 clear alpha + DWM composition below.
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);

    // MARGINS{-1}: extend DWM glass over the entire client area.
    // This is the call that makes alpha=0 pixels transparent instead of black.
    MARGINS m{ -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(hwnd, &m);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    if (!init_dx11(width, height)) return false;
    if (!create_rtv())             return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    ImGui::StyleColorsDark();
    ImGui::GetStyle().Alpha          = 0.95f;
    ImGui::GetStyle().WindowRounding = 4.0f;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(device, context);

    return true;
}

void Overlay::update_visibility_and_input() {
    // Hide overlay when user alt-tabs away from CS2.
    // g_cs2_hwnd is CS2's SDL_app window — checked against GetForegroundWindow.
    HWND fg = GetForegroundWindow();
    bool cs2_focused = (fg == g_cs2_hwnd || fg == hwnd);

    if (!cs2_focused) {
        // Not in CS2 — hide entirely, no flicker on other apps
        if (IsWindowVisible(hwnd))
            ShowWindow(hwnd, SW_HIDE);
        return;
    }

    if (!IsWindowVisible(hwnd))
        ShowWindow(hwnd, SW_SHOW);

    // Toggle click-through based on menu state
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (g_menu_open) {
        ex &= ~WS_EX_TRANSPARENT;
        ex &= ~WS_EX_NOACTIVATE;
        SetWindowLongW(hwnd, GWL_EXSTYLE, ex);
        SetForegroundWindow(hwnd);
    } else {
        ex |= WS_EX_TRANSPARENT;
        ex |= WS_EX_NOACTIVATE;
        SetWindowLongW(hwnd, GWL_EXSTYLE, ex);
    }
}

bool Overlay::init_dx11(int width, int height) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount                        = 2;
    sd.BufferDesc.Width                   = (UINT)width;
    sd.BufferDesc.Height                  = (UINT)height;
    sd.BufferDesc.Format                  = DXGI_FORMAT_B8G8R8A8_UNORM; // BGRA — required for DWM alpha
    sd.BufferDesc.RefreshRate.Numerator   = 0;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags                              = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow                       = hwnd;
    sd.SampleDesc.Count                   = 1;
    sd.Windowed                           = TRUE;
    sd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL level;
    constexpr D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };

    return SUCCEEDED(D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        0, levels, 1, D3D11_SDK_VERSION,
        &sd, &swapchain, &device, &level, &context
    ));
}

bool Overlay::create_rtv() {
    ID3D11Texture2D* buf = nullptr;
    swapchain->GetBuffer(0, IID_PPV_ARGS(&buf));
    if (!buf) return false;
    device->CreateRenderTargetView(buf, nullptr, &rtv);
    buf->Release();
    return rtv != nullptr;
}

void Overlay::release_rtv() {
    if (rtv) { rtv->Release(); rtv = nullptr; }
}

void Overlay::begin_frame() {
    update_visibility_and_input();
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void Overlay::end_frame() {
    ImGui::Render();

    // Clear to fully transparent — DWM composites only the ImGui pixels.
    constexpr float clear[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context->OMSetRenderTargets(1, &rtv, nullptr);
    context->ClearRenderTargetView(rtv, clear);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swapchain->Present(1, 0);
}

void Overlay::cleanup() {
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    release_rtv();
    if (swapchain) { swapchain->Release(); swapchain = nullptr; }
    if (context)   { context->Release();   context   = nullptr; }
    if (device)    { device->Release();    device    = nullptr; }
    if (hwnd)      { DestroyWindow(hwnd);  hwnd      = nullptr; }
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}
