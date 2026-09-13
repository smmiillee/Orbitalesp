// --- src/overlay.cpp ---
#include "overlay.h"
#include <dwmapi.h>
#include <imgui.h>
#include <imgui_impl_win32.h>
#include <imgui_impl_dx11.h>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(HWND, UINT, WPARAM, LPARAM);
extern bool g_menu_open;
extern bool g_vsync;
extern HWND g_cs2_hwnd;

LRESULT CALLBACK Overlay::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    // Eat WM_SETCURSOR so Windows never changes it to the loading arrow
    if (msg == WM_SETCURSOR) { SetCursor(LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW)); return TRUE; }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

bool Overlay::create(int width, int height) {
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = wnd_proc;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW); // arrow, never hourglass
    wc.lpszClassName = L"OrbitalESP_Overlay";
    if (!RegisterClassExW(&wc)) return false;

    hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE
        | WS_EX_TOOLWINDOW,  // <-- hides from taskbar and alt-tab list
        wc.lpszClassName, L"OrbitalESP",
        WS_POPUP,
        0, 0, width, height,
        nullptr, nullptr, wc.hInstance, nullptr
    );
    if (!hwnd) return false;

    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);

    MARGINS m{ -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(hwnd, &m);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    if (!init_dx11(width, height)) return false;
    if (!create_rtv())             return false;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange; // we manage cursor

    // ── BramptonHook-style theme ──────────────────────────────────────────────
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding    = 0.0f;
    s.ChildRounding     = 0.0f;
    s.FrameRounding     = 0.0f;
    s.GrabRounding      = 0.0f;
    s.PopupRounding     = 0.0f;
    s.ScrollbarRounding = 0.0f;
    s.TabRounding       = 0.0f;
    s.WindowBorderSize  = 1.0f;
    s.FrameBorderSize   = 1.0f;
    s.WindowPadding     = { 8.0f, 6.0f };
    s.FramePadding      = { 4.0f, 3.0f };
    s.ItemSpacing       = { 6.0f, 4.0f };
    s.ItemInnerSpacing  = { 4.0f, 4.0f };
    s.IndentSpacing     = 14.0f;
    s.ScrollbarSize     = 12.0f;
    s.GrabMinSize       = 8.0f;

    // Classic Win95/early-2000s cheat panel palette
    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg]           = { 0.78f, 0.78f, 0.78f, 0.97f }; // light grey bg
    c[ImGuiCol_ChildBg]            = { 0.82f, 0.82f, 0.82f, 1.00f };
    c[ImGuiCol_PopupBg]            = { 0.80f, 0.80f, 0.80f, 1.00f };
    c[ImGuiCol_Border]             = { 0.30f, 0.30f, 0.30f, 1.00f };
    c[ImGuiCol_BorderShadow]       = { 0.00f, 0.00f, 0.00f, 0.00f };
    c[ImGuiCol_FrameBg]            = { 1.00f, 1.00f, 1.00f, 1.00f }; // white input boxes
    c[ImGuiCol_FrameBgHovered]     = { 0.90f, 0.90f, 0.90f, 1.00f };
    c[ImGuiCol_FrameBgActive]      = { 0.85f, 0.85f, 0.85f, 1.00f };
    c[ImGuiCol_TitleBg]            = { 0.00f, 0.00f, 0.50f, 1.00f }; // dark blue title
    c[ImGuiCol_TitleBgActive]      = { 0.00f, 0.00f, 0.70f, 1.00f };
    c[ImGuiCol_TitleBgCollapsed]   = { 0.00f, 0.00f, 0.40f, 1.00f };
    c[ImGuiCol_MenuBarBg]          = { 0.75f, 0.75f, 0.75f, 1.00f };
    c[ImGuiCol_ScrollbarBg]        = { 0.75f, 0.75f, 0.75f, 1.00f };
    c[ImGuiCol_ScrollbarGrab]      = { 0.50f, 0.50f, 0.50f, 1.00f };
    c[ImGuiCol_ScrollbarGrabHovered] = { 0.40f, 0.40f, 0.40f, 1.00f };
    c[ImGuiCol_ScrollbarGrabActive]  = { 0.30f, 0.30f, 0.30f, 1.00f };
    c[ImGuiCol_CheckMark]          = { 0.00f, 0.00f, 0.00f, 1.00f }; // black checkmark
    c[ImGuiCol_SliderGrab]         = { 0.55f, 0.55f, 0.55f, 1.00f };
    c[ImGuiCol_SliderGrabActive]   = { 0.35f, 0.35f, 0.35f, 1.00f };
    c[ImGuiCol_Button]             = { 0.88f, 0.80f, 0.55f, 1.00f }; // tan/gold buttons
    c[ImGuiCol_ButtonHovered]      = { 0.95f, 0.88f, 0.65f, 1.00f };
    c[ImGuiCol_ButtonActive]       = { 0.75f, 0.68f, 0.45f, 1.00f };
    c[ImGuiCol_Header]             = { 0.70f, 0.70f, 0.70f, 1.00f };
    c[ImGuiCol_HeaderHovered]      = { 0.65f, 0.65f, 0.65f, 1.00f };
    c[ImGuiCol_HeaderActive]       = { 0.60f, 0.60f, 0.60f, 1.00f };
    c[ImGuiCol_Separator]          = { 0.40f, 0.40f, 0.40f, 1.00f };
    c[ImGuiCol_SeparatorHovered]   = { 0.30f, 0.30f, 0.30f, 1.00f };
    c[ImGuiCol_SeparatorActive]    = { 0.20f, 0.20f, 0.20f, 1.00f };
    c[ImGuiCol_ResizeGrip]         = { 0.60f, 0.60f, 0.60f, 1.00f };
    c[ImGuiCol_ResizeGripHovered]  = { 0.50f, 0.50f, 0.50f, 1.00f };
    c[ImGuiCol_ResizeGripActive]   = { 0.40f, 0.40f, 0.40f, 1.00f };
    c[ImGuiCol_Tab]                = { 0.75f, 0.75f, 0.75f, 1.00f };
    c[ImGuiCol_TabHovered]         = { 0.85f, 0.85f, 0.85f, 1.00f };
    c[ImGuiCol_TabActive]          = { 0.90f, 0.90f, 0.90f, 1.00f };
    c[ImGuiCol_Text]               = { 0.00f, 0.00f, 0.00f, 1.00f }; // black text
    c[ImGuiCol_TextDisabled]       = { 0.45f, 0.45f, 0.45f, 1.00f };

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(device, context);

    return true;
}

void Overlay::update_visibility_and_input() {
    HWND fg = GetForegroundWindow();
    bool cs2_focused = (fg == g_cs2_hwnd || fg == hwnd);

    if (!cs2_focused) {
        if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
        return;
    }
    if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOW);

    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (g_menu_open) {
        ex &= ~WS_EX_TRANSPARENT;
        ex &= ~WS_EX_NOACTIVATE;
        SetWindowLongW(hwnd, GWL_EXSTYLE, ex);
        SetCursor(LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW));
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
    sd.BufferDesc.Format                  = DXGI_FORMAT_B8G8R8A8_UNORM;
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
