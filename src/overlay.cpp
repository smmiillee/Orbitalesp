// --- src/overlay.cpp ---
//
// DWM per-pixel alpha overlay, parented to CS2's SDL_app window.
//
// How it works:
//   1. We find CS2's HWND and get its client rect.
//   2. We create a WS_CHILD window parented to CS2 — DWM composites
//      it on top with correct per-pixel alpha. No separate topmost window.
//   3. The child window is WS_EX_TRANSPARENT + WS_EX_LAYERED when the
//      menu is closed (clicks fall through to CS2). When INSERT is pressed
//      those styles are stripped so ImGui receives mouse input.
//   4. Because it's a child of CS2, it automatically hides when CS2 is
//      minimized or loses focus. No extra logic needed.
//   5. DX11 clears to alpha=0 every frame — transparent pixels are truly
//      transparent via DWM, no colorkey tricks.
//
#include "overlay.h"
#include <dwmapi.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
extern bool g_menu_open; // defined in main.cpp

LRESULT CALLBACK Overlay::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    // Don't handle WM_DESTROY — child window lifetime is tied to CS2's window.
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool Overlay::create(HWND cs2_hwnd, int width, int height) {
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.lpszClassName = L"OrbitalESP_DWM";
    if (!RegisterClassExW(&wc)) return false;

    // WS_CHILD: parented to CS2 — follows it on minimize/alt-tab/focus.
    // WS_EX_LAYERED: required for per-pixel alpha via DWM.
    // WS_EX_TRANSPARENT: click-through by default; removed when menu opens.
    // WS_EX_NOACTIVATE: don't steal focus from CS2 when click-through is off.
    hwnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE,
        wc.lpszClassName, L"OrbitalESP",
        WS_CHILD | WS_VISIBLE,
        0, 0, width, height,
        cs2_hwnd,                   // <-- parented to CS2
        nullptr,
        wc.hInstance,
        nullptr
    );
    if (!hwnd) return false;

    // LWA_ALPHA 255: window is fully opaque at the Win32 level.
    // Actual transparency comes from DX11 clear color alpha=0 + DWM composition.
    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);

    // Tell DWM to extend the glass frame over the entire client area.
    // This is what makes per-pixel alpha work on a child window.
    MARGINS m{ -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(hwnd, &m);

    if (!init_dx11(width, height)) return false;
    if (!create_rtv())             return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    ImGui::StyleColorsDark();
    ImGui::GetStyle().Alpha        = 0.95f;
    ImGui::GetStyle().WindowRounding = 4.0f;

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(device, context);

    return true;
}

void Overlay::update_input_mode() {
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (g_menu_open) {
        // Remove click-through so ImGui gets mouse/keyboard
        ex &= ~WS_EX_TRANSPARENT;
        // Keep NOACTIVATE off too so clicks land on our window
        ex &= ~WS_EX_NOACTIVATE;
    } else {
        // Restore click-through — all input goes to CS2
        ex |= WS_EX_TRANSPARENT;
        ex |= WS_EX_NOACTIVATE;
    }
    SetWindowLongW(hwnd, GWL_EXSTYLE, ex);
}

bool Overlay::init_dx11(int width, int height) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount                        = 2;
    sd.BufferDesc.Width                   = (UINT)width;
    sd.BufferDesc.Height                  = (UINT)height;
    sd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
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
    update_input_mode();
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
}

void Overlay::end_frame() {
    ImGui::Render();

    // Alpha = 0.0 — DWM sees transparent pixels and composites through to CS2.
    // Only pixels touched by ImGui draw calls are opaque.
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
