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
extern void OrbitalLog(const char* fmt, ...);

LRESULT CALLBACK Overlay::wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (ImGui_ImplWin32_WndProcHandler(hwnd, msg, wp, lp)) return true;
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    if (msg == WM_SETCURSOR) {
        SetCursor(LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW));
        return TRUE;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void Overlay::enable_dpi_awareness() {
    using SetDpiCtxFn = BOOL (WINAPI*)(HANDLE);
    if (HMODULE user32 = GetModuleHandleW(L"user32.dll")) {
        auto fn = reinterpret_cast<SetDpiCtxFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 == (HANDLE)-4
        if (fn && fn(reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4))))
            return;
    }
    SetProcessDPIAware();
}

bool Overlay::create(int width, int height) {
    wc.cbSize = sizeof(wc);
    wc.style = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandleW(nullptr);
    wc.hCursor = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"OrbitalESP_Overlay";

    if (!RegisterClassExW(&wc)) {
        OrbitalLog("RegisterClassExW failed err=%lu", GetLastError());
        return false;
    }

    hwnd = CreateWindowExW(
        WS_EX_TOPMOST | WS_EX_LAYERED | WS_EX_TRANSPARENT | WS_EX_NOACTIVATE
        | WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"OrbitalESP",
        WS_POPUP,
        0, 0, width, height,
        nullptr, nullptr, wc.hInstance, nullptr
    );
    if (!hwnd) {
        OrbitalLog("CreateWindowExW failed err=%lu", GetLastError());
        return false;
    }

    SetLayeredWindowAttributes(hwnd, 0, 255, LWA_ALPHA);

    MARGINS m{ -1, -1, -1, -1 };
    DwmExtendFrameIntoClientArea(hwnd, &m);

    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    if (!init_dx11(width, height)) return false;
    if (!create_rtv())            return false;

    width_  = width;
    height_ = height;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();

    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;

    // ── early-2000s cheat panel theme ─────────────────────────────────────
    ImGuiStyle& s = ImGui::GetStyle();
    s.WindowRounding = 0.0f;
    s.ChildRounding = 0.0f;
    s.FrameRounding = 0.0f;
    s.GrabRounding = 0.0f;
    s.PopupRounding = 0.0f;
    s.ScrollbarRounding = 0.0f;
    s.TabRounding = 0.0f;
    s.WindowBorderSize = 1.0f;
    s.FrameBorderSize = 1.0f;
    s.WindowPadding = { 8.0f, 6.0f };
    s.FramePadding = { 4.0f, 3.0f };
    s.ItemSpacing = { 6.0f, 4.0f };
    s.ItemInnerSpacing = { 4.0f, 4.0f };
    s.IndentSpacing = 14.0f;
    s.ScrollbarSize = 12.0f;
    s.GrabMinSize = 8.0f;

    ImVec4* c = s.Colors;
    c[ImGuiCol_WindowBg] = { 0.78f, 0.78f, 0.78f, 0.97f };
    c[ImGuiCol_ChildBg] = { 0.82f, 0.82f, 0.82f, 1.00f };
    c[ImGuiCol_PopupBg] = { 0.80f, 0.80f, 0.80f, 1.00f };
    c[ImGuiCol_Border] = { 0.30f, 0.30f, 0.30f, 1.00f };
    c[ImGuiCol_BorderShadow] = { 0.00f, 0.00f, 0.00f, 0.00f };
    c[ImGuiCol_FrameBg] = { 1.00f, 1.00f, 1.00f, 1.00f };
    c[ImGuiCol_FrameBgHovered] = { 0.90f, 0.90f, 0.90f, 1.00f };
    c[ImGuiCol_FrameBgActive] = { 0.85f, 0.85f, 0.85f, 1.00f };
    c[ImGuiCol_TitleBg] = { 0.00f, 0.00f, 0.50f, 1.00f };
    c[ImGuiCol_TitleBgActive] = { 0.00f, 0.00f, 0.70f, 1.00f };
    c[ImGuiCol_TitleBgCollapsed] = { 0.00f, 0.00f, 0.40f, 1.00f };
    c[ImGuiCol_MenuBarBg] = { 0.75f, 0.75f, 0.75f, 1.00f };
    c[ImGuiCol_ScrollbarBg] = { 0.75f, 0.75f, 0.75f, 1.00f };
    c[ImGuiCol_ScrollbarGrab] = { 0.50f, 0.50f, 0.50f, 1.00f };
    c[ImGuiCol_ScrollbarGrabHovered] = { 0.40f, 0.40f, 0.40f, 1.00f };
    c[ImGuiCol_ScrollbarGrabActive] = { 0.30f, 0.30f, 0.30f, 1.00f };
    c[ImGuiCol_CheckMark] = { 0.00f, 0.00f, 0.00f, 1.00f };
    c[ImGuiCol_SliderGrab] = { 0.55f, 0.55f, 0.55f, 1.00f };
    c[ImGuiCol_SliderGrabActive] = { 0.35f, 0.35f, 0.35f, 1.00f };
    c[ImGuiCol_Button] = { 0.88f, 0.80f, 0.55f, 1.00f };
    c[ImGuiCol_ButtonHovered] = { 0.95f, 0.88f, 0.65f, 1.00f };
    c[ImGuiCol_ButtonActive] = { 0.75f, 0.68f, 0.45f, 1.00f };
    c[ImGuiCol_Header] = { 0.70f, 0.70f, 0.70f, 1.00f };
    c[ImGuiCol_HeaderHovered] = { 0.65f, 0.65f, 0.65f, 1.00f };
    c[ImGuiCol_HeaderActive] = { 0.60f, 0.60f, 0.60f, 1.00f };
    c[ImGuiCol_Separator] = { 0.40f, 0.40f, 0.40f, 1.00f };
    c[ImGuiCol_SeparatorHovered] = { 0.30f, 0.30f, 0.30f, 1.00f };
    c[ImGuiCol_SeparatorActive] = { 0.20f, 0.20f, 0.20f, 1.00f };
    c[ImGuiCol_ResizeGrip] = { 0.60f, 0.60f, 0.60f, 1.00f };
    c[ImGuiCol_ResizeGripHovered] = { 0.50f, 0.50f, 0.50f, 1.00f };
    c[ImGuiCol_ResizeGripActive] = { 0.40f, 0.40f, 0.40f, 1.00f };
    c[ImGuiCol_Tab] = { 0.75f, 0.75f, 0.75f, 1.00f };
    c[ImGuiCol_TabHovered] = { 0.85f, 0.85f, 0.85f, 1.00f };
    c[ImGuiCol_TabActive] = { 0.90f, 0.90f, 0.90f, 1.00f };
    c[ImGuiCol_Text] = { 0.00f, 0.00f, 0.00f, 1.00f };
    c[ImGuiCol_TextDisabled] = { 0.45f, 0.45f, 0.45f, 1.00f };

    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(device, context);

    OrbitalLog("imgui initialised, overlay hwnd=0x%p", hwnd);
    return true;
}

// Glue the overlay to the game's client area, and resize the swap chain if the
// game's resolution changed.
void Overlay::sync_to_game() {
    if (!hwnd) return;
    if (!g_cs2_hwnd || !IsWindow(g_cs2_hwnd)) return;

    RECT rc{};
    if (!GetClientRect(g_cs2_hwnd, &rc)) return;

    POINT tl{ 0, 0 };
    if (!ClientToScreen(g_cs2_hwnd, &tl)) return;

    const int w = rc.right - rc.left;
    const int h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;

    RECT cur{};
    GetWindowRect(hwnd, &cur);
    if (cur.left != tl.x || cur.top != tl.y ||
        (cur.right - cur.left) != w || (cur.bottom - cur.top) != h) {
        SetWindowPos(hwnd, HWND_TOPMOST, tl.x, tl.y, w, h, SWP_NOACTIVATE);
    }

    if (w != width_ || h != height_) resize_buffers(w, h);
}

bool Overlay::resize_buffers(int width, int height) {
    if (!swapchain || !context) return false;

    context->OMSetRenderTargets(0, nullptr, nullptr);
    release_rtv();

    if (FAILED(swapchain->ResizeBuffers(0, (UINT)width, (UINT)height,
                                        DXGI_FORMAT_UNKNOWN, 0)))
        return false;

    width_  = width;
    height_ = height;
    return create_rtv();
}

void Overlay::update_visibility_and_input() {
    if (!hwnd) return;

    // *** THIS IS WHAT COULD HIDE THE OVERLAY FOREVER ***
    // The old logic hid the window whenever the game was not the foreground
    // window. If g_cs2_hwnd was missing or stale, that condition was ALWAYS
    // true, so the window was hidden on its very first frame and never came
    // back -- process alive, nothing drawn, INSERT apparently dead.
    //
    // Now we only hide when we actually HAVE a live game window and it is not
    // focused, and never while the menu is open.
    const bool have_game = (g_cs2_hwnd && IsWindow(g_cs2_hwnd));
    if (have_game) {
        const HWND fg = GetForegroundWindow();
        const bool focused = (fg == g_cs2_hwnd || fg == hwnd);
        if (!focused && !g_menu_open) {
            if (IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_HIDE);
            return;
        }
    }

    if (!IsWindowVisible(hwnd)) ShowWindow(hwnd, SW_SHOW);

    // The overlay NEVER activates. It keeps WS_EX_NOACTIVATE at all times and
    // only toggles click-through, so CS2 retains keyboard focus even while the
    // menu is open. WS_EX_NOACTIVATE still allows mouse clicks, so the menu
    // stays fully usable.
    LONG ex = GetWindowLongW(hwnd, GWL_EXSTYLE);
    if (g_menu_open) {
        ex &= ~WS_EX_TRANSPARENT;
        SetCursor(LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW));
    } else {
        ex |= WS_EX_TRANSPARENT;
    }
    ex |= WS_EX_NOACTIVATE;
    SetWindowLongW(hwnd, GWL_EXSTYLE, ex);
}

bool Overlay::init_dx11(int width, int height) {
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount = 2;
    sd.BufferDesc.Width = (UINT)width;
    sd.BufferDesc.Height = (UINT)height;
    sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator = 0;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow = hwnd;
    sd.SampleDesc.Count = 1;
    sd.Windowed = TRUE;
    sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL level;
    constexpr D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };

    const HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        0, levels, 1, D3D11_SDK_VERSION,
        &sd, &swapchain, &device, &level, &context
    );
    if (FAILED(hr)) {
        OrbitalLog("D3D11CreateDeviceAndSwapChain failed hr=0x%08lX",
                   static_cast<unsigned long>(hr));
        return false;
    }
    return true;
}

bool Overlay::create_rtv() {
    if (!swapchain || !device) return false;

    ID3D11Texture2D* buf = nullptr;
    if (FAILED(swapchain->GetBuffer(0, IID_PPV_ARGS(&buf))) || !buf) {
        OrbitalLog("swapchain->GetBuffer failed");
        return false;
    }
    const HRESULT hr = device->CreateRenderTargetView(buf, nullptr, &rtv);
    buf->Release();

    if (FAILED(hr) || !rtv) {
        OrbitalLog("CreateRenderTargetView failed hr=0x%08lX",
                   static_cast<unsigned long>(hr));
        return false;
    }
    return true;
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

    // Present used to be hardcoded as Present(1, 0) -- vsync forced on -- which
    // locked the overlay to the refresh rate while the game ran far faster, so
    // every box was drawn with a view matrix up to a full refresh stale.
    swapchain->Present(g_vsync ? 1 : 0, 0);
}

void Overlay::cleanup() {
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    release_rtv();
    if (swapchain) { swapchain->Release(); swapchain = nullptr; }
    if (context) { context->Release(); context = nullptr; }
    if (device) { device->Release(); device = nullptr; }
    if (hwnd) { DestroyWindow(hwnd); hwnd = nullptr; }
    UnregisterClassW(wc.lpszClassName, wc.hInstance);
}
