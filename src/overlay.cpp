#include "overlay.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <dwmapi.h>
#include <cstdio>

extern IMGUI_IMPL_API LRESULT ImGui_ImplWin32_WndProcHandler(
    HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam);

static OverlayContext* g_Ctx = nullptr;

static LRESULT CALLBACK WndProc(HWND hWnd, UINT msg, WPARAM wParam, LPARAM lParam) {
    if (ImGui_ImplWin32_WndProcHandler(hWnd, msg, wParam, lParam))
        return true;
    if (msg == WM_DESTROY) { PostQuitMessage(0); return 0; }
    return DefWindowProcA(hWnd, msg, wParam, lParam);
}

bool OverlayCreate(OverlayContext& ctx) {
    g_Ctx = &ctx;

    // Open a console so errors are visible if overlay fails
    AllocConsole();
    FILE* f;
    freopen_s(&f, "CONOUT$", "w", stdout);
    printf("[*] Overlay starting...\n");

    // Match CS2 window size
    HWND cs2 = FindWindowA("SDL_app", nullptr);
    if (cs2) {
        RECT r{};
        GetClientRect(cs2, &r);
        if (r.right > 0) {
            ctx.width  = r.right;
            ctx.height = r.bottom;
        }
        printf("[*] CS2 window found: %dx%d\n", ctx.width, ctx.height);
    } else {
        printf("[!] CS2 window NOT found — using default 1920x1080\n");
        printf("[!] Make sure CS2 is running before launching this.\n");
    }

    // Register window class
    WNDCLASSEXA wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.lpszClassName = "CS2ExternalOverlay";
    wc.hInstance     = GetModuleHandleA(nullptr);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    if (!RegisterClassExA(&wc)) {
        printf("[!] RegisterClassExA failed: %lu\n", GetLastError());
        return false;
    }

    ctx.hwnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        wc.lpszClassName,
        "CS2Overlay",
        WS_POPUP,
        0, 0, ctx.width, ctx.height,
        nullptr, nullptr, wc.hInstance, nullptr
    );

    if (!ctx.hwnd) {
        printf("[!] CreateWindowExA failed: %lu\n", GetLastError());
        return false;
    }
    printf("[*] Window created OK\n");

    // Layered window — fully opaque but composited via DWM
    SetLayeredWindowAttributes(ctx.hwnd, RGB(0, 0, 0), 255, LWA_ALPHA);
    MARGINS mg = {-1, -1, -1, -1};
    DwmExtendFrameIntoClientArea(ctx.hwnd, &mg);

    // D3D11 device + swap chain
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount                        = 2;
    sd.BufferDesc.Width                   = (UINT)ctx.width;
    sd.BufferDesc.Height                  = (UINT)ctx.height;
    sd.BufferDesc.Format                  = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.BufferDesc.RefreshRate.Numerator   = 144;
    sd.BufferDesc.RefreshRate.Denominator = 1;
    sd.Flags                              = DXGI_SWAP_CHAIN_FLAG_ALLOW_MODE_SWITCH;
    sd.BufferUsage                        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.OutputWindow                       = ctx.hwnd;
    sd.SampleDesc.Count                   = 1;
    sd.Windowed                           = TRUE;
    sd.SwapEffect                         = DXGI_SWAP_EFFECT_DISCARD;

    D3D_FEATURE_LEVEL featureLevel;
    const D3D_FEATURE_LEVEL levels[] = { D3D_FEATURE_LEVEL_11_0 };

    HRESULT hr = D3D11CreateDeviceAndSwapChain(
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
        0, levels, 1, D3D11_SDK_VERSION,
        &sd, &ctx.swapChain,
        &ctx.device, &featureLevel, &ctx.deviceCtx
    );

    if (FAILED(hr)) {
        printf("[!] D3D11CreateDeviceAndSwapChain failed: 0x%08lX\n", hr);
        return false;
    }
    printf("[*] D3D11 device created OK\n");

    // Render target view
    ID3D11Texture2D* backBuf = nullptr;
    ctx.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuf));
    ctx.device->CreateRenderTargetView(backBuf, nullptr, &ctx.rtv);
    backBuf->Release();

    // Show window
    ShowWindow(ctx.hwnd, SW_SHOWNOACTIVATE);
    UpdateWindow(ctx.hwnd);

    // ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr;
    io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    ImGui::StyleColorsDark();

    // Tweak menu style
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding    = 6.0f;
    style.FrameRounding     = 3.0f;
    style.Colors[ImGuiCol_WindowBg] = ImVec4(0.08f, 0.08f, 0.08f, 0.90f);

    ImGui_ImplWin32_Init(ctx.hwnd);
    ImGui_ImplDX11_Init(ctx.device, ctx.deviceCtx);

    printf("[*] ImGui initialized OK\n");
    printf("[*] Overlay ready. INSERT = menu, F9 = exit.\n");
    return true;
}

void OverlayRun(OverlayContext& ctx, std::function<void()> renderFn) {
    MSG msg{};

    while (ctx.running) {
        // Pump messages
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
            if (msg.message == WM_QUIT) ctx.running = false;
        }

        // INSERT toggles menu
        static bool insertLast = false;
        bool insertNow = (GetAsyncKeyState(VK_INSERT) & 0x8000) != 0;
        if (insertNow && !insertLast) {
            ctx.menuOpen = !ctx.menuOpen;

            // When menu is open: accept mouse input, remove click-through
            // When menu is closed: restore click-through
            LONG exStyle = GetWindowLongA(ctx.hwnd, GWL_EXSTYLE);
            if (ctx.menuOpen) {
                exStyle &= ~WS_EX_TRANSPARENT;
                exStyle &= ~WS_EX_NOACTIVATE;
            } else {
                exStyle |= WS_EX_TRANSPARENT;
                exStyle |= WS_EX_NOACTIVATE;
            }
            SetWindowLongA(ctx.hwnd, GWL_EXSTYLE, exStyle);
            if (ctx.menuOpen) SetForegroundWindow(ctx.hwnd);
        }
        insertLast = insertNow;

        // F9 kills the process
        if (GetAsyncKeyState(VK_F9) & 0x8000) {
            ctx.running = false;
            break;
        }

        // Keep overlay on top of CS2
        HWND cs2 = FindWindowA("SDL_app", nullptr);
        if (cs2) {
            SetWindowPos(ctx.hwnd, HWND_TOPMOST,
                0, 0, ctx.width, ctx.height,
                SWP_NOMOVE | SWP_NOSIZE | SWP_NOACTIVATE);
        }

        // Frame
        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        renderFn();

        ImGui::Render();
        const float clear[4] = {0.0f, 0.0f, 0.0f, 0.0f};
        ctx.deviceCtx->OMSetRenderTargets(1, &ctx.rtv, nullptr);
        ctx.deviceCtx->ClearRenderTargetView(ctx.rtv, clear);
        ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
        ctx.swapChain->Present(1, 0);
    }
}

void OverlayDestroy(OverlayContext& ctx) {
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
    if (ctx.rtv)       { ctx.rtv->Release();       ctx.rtv = nullptr; }
    if (ctx.swapChain) { ctx.swapChain->Release();  ctx.swapChain = nullptr; }
    if (ctx.deviceCtx) { ctx.deviceCtx->Release();  ctx.deviceCtx = nullptr; }
    if (ctx.device)    { ctx.device->Release();     ctx.device = nullptr; }
    if (ctx.hwnd)      { DestroyWindow(ctx.hwnd);   ctx.hwnd = nullptr; }
}
