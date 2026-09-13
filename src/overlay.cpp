// --- src/overlay.cpp ---
#include "overlay.h"
#include <dwmapi.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);

// Shared flag — main.cpp sets this when INSERT is toggled
extern bool g_menu_open;

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
    wc.lpszClassName = L"OrbitalESP";
    RegisterClassExW(&wc);

    // WS_EX_TRANSPARENT removed here — we toggle it dynamically.
    // WS_EX_NOACTIVATE removed — ImGui needs to be activatable when menu is open.
    hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED,
        wc.lpszClassName, L"OrbitalESP",
        WS_POPUP,
        0, 0, width, height,
        nullptr, nullptr, wc.hInstance, nullptr
    );
    if (!hwnd) return false;

    // LWA_ALPHA at 255 = fully opaque window, but DX11 alpha channel
    // (clear color alpha = 0.0) punches through correctly via DWM composition.
    // LWA_COLORKEY on black was eating every black pixel including ImGui shadows.
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    if (!create_device())        return false;
    if (!create_render_target()) return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    ImGui::StyleColorsDark();
    // Make menu slightly more visible against game background
    ImGui::GetStyle().Alpha = 0.92f;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(device, context);

    return true;
}

// Call this every frame BEFORE begin_frame so the window style is current.
// When menu opens: remove click-through, bring window to foreground for input.
// When menu closes: restore click-through, game gets all input back.
void Overlay::update_input_mode() {
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);

    if (g_menu_open) {
        // Remove click-through + no-activate so ImGui receives mouse/keyboard
        ex &= ~WS_EX_TRANSPARENT;
        ex &= ~WS_EX_NOACTIVATE;
        SetWindowLongW(hwnd, GWL_EXSTYLE, ex);
        SetForegroundWindow(hwnd);
    } else {
        // Restore click-through — game gets all input
        ex |= WS_EX_TRANSPARENT;
        ex |= WS_EX_NOACTIVATE;
        SetWindowLongW(hwnd, GWL_EXSTYLE, ex);
    }
}

bool Overlay::create_device() {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount                        = 2;
    sd.BufferDesc.Width                   = 0;
    sd.BufferDesc.Height                  = 0;
    sd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 144;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags                              = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow                       = hwnd;
    sd.SampleDesc.Count                   = 1;
    sd.Windowed                           = TRUE;
    sd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL feature_level;
    constexpr D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        0, levels, 1, D3D11_SDK_VERSION,
        &sd, &swapchain, &device, &feature_level, &context
    );
    return SUCCEEDED(hr);
}

bool Overlay::create_render_target() {
    ID3D11Texture2D* back_buffer = nullptr;
    swapchain->GetBuffer(0, IID_PPV_ARGS(&back_buffer));
    if (!back_buffer) return false;
    device->CreateRenderTargetView(back_buffer, nullptr, &rtv);
    back_buffer->Release();
    return rtv != nullptr;
}

void Overlay::release_render_target() {
    if (rtv) { rtv->Release(); rtv = nullptr; }
}

void Overlay::begin_frame() {
    update_input_mode();
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void Overlay::end_frame() {
    ImGui::Render();

    // Alpha = 0.0 — DX11 renders fully transparent background.
    // Only ImGui draw calls produce visible pixels.
    constexpr float clear_color[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    context->OMSetRenderTargets(1, &rtv, nullptr);
    context->ClearRenderTargetView(rtv, clear_color);
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    swapchain->Present(1, 0);
}

void Overlay::cleanup() {
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    release_render_target();
    if (swapchain) { swapchain->Release(); swapchain = nullptr; }
    if (context)   { context->Release();   context   = nullptr; }
    if (device)    { device->Release();    device    = nullptr; }
    if (hwnd)      { DestroyWindow(hwnd);  hwnd      = nullptr; }
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}
