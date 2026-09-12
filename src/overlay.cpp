#include "overlay.h"
#include "imgui.h"
#include "imgui_impl_win32.h"
#include "imgui_impl_dx11.h"
#include <dwmapi.h>
#include <stdexcept>

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

    // Find CS2 window to match its size
    HWND cs2 = FindWindowA("SDL_app", nullptr);
    if (cs2) {
        RECT r{};
        GetClientRect(cs2, &r);
        if (r.right > 0) { ctx.width = r.right; ctx.height = r.bottom; }
    }

    WNDCLASSEXA wc{};
    wc.cbSize        = sizeof(wc);
    wc.lpfnWndProc   = WndProc;
    wc.lpszClassName = "CS2ExternalOverlay";
    wc.hInstance     = GetModuleHandleA(nullptr);
    RegisterClassExA(&wc);

    ctx.hwnd = CreateWindowExA(
        WS_EX_TOPMOST | WS_EX_TRANSPARENT | WS_EX_LAYERED | WS_EX_NOACTIVATE,
        wc.lpszClassName, "overlay",
        WS_POPUP,
        0, 0, ctx.width, ctx.height,
        nullptr, nullptr, wc.hInstance, nullptr
    );
    if (!ctx.hwnd) return false;

    // Transparent background via DWM
    SetLayeredWindowAttributes(ctx.hwnd, RGB(0,0,0), 255, LWA_ALPHA);
    MARGINS mg = {-1};
    DwmExtendFrameIntoClientArea(ctx.hwnd, &mg);

    // D3D11 swap chain
    DXGI_SWAP_CHAIN_DESC sd{};
    sd.BufferCount                        = 2;
    sd.BufferDesc.Width                   = ctx.width;
    sd.BufferDesc.Height                  = ctx.height;
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
        nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, 0,
        levels, 1, D3D11_SDK_VERSION,
        &sd, &ctx.swapChain,
        &ctx.device, &featureLevel, &ctx.deviceCtx
    );
    if (FAILED(hr)) return false;

    // RTV
    ID3D11Texture2D* backBuf = nullptr;
    ctx.swapChain->GetBuffer(0, IID_PPV_ARGS(&backBuf));
    ctx.device->CreateRenderTargetView(backBuf, nullptr, &ctx.rtv);
    backBuf->Release();

    ShowWindow(ctx.hwnd, SW_SHOW);
    UpdateWindow(ctx.hwnd);

    // ImGui
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGui::GetIO().IniFilename = nullptr;
    ImGui::StyleColorsDark();
    ImGui_ImplWin32_Init(ctx.hwnd);
    ImGui_ImplDX11_Init(ctx.device, ctx.deviceCtx);

    return true;
}

void OverlayRun(OverlayContext& ctx, std::function<void()> renderFn) {
    MSG msg{};
    while (ctx.running) {
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageA(&msg);
            if (msg.message == WM_QUIT) { ctx.running = false; }
        }

        // Keep overlay on top of CS2 at all times
        HWND cs2 = FindWindowA("SDL_app", nullptr);
        if (cs2) {
            HWND fg = GetForegroundWindow();
            if (fg == cs2 || fg == ctx.hwnd) {
                SetWindowPos(ctx.hwnd, HWND_TOPMOST, 0, 0,
                             ctx.width, ctx.height, SWP_NOMOVE | SWP_NOSIZE);
            }
        }

        ImGui_ImplDX11_NewFrame();
        ImGui_ImplWin32_NewFrame();
        ImGui::NewFrame();

        renderFn();

        ImGui::Render();
        constexpr float clear[4] = {0,0,0,0};
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
    if (ctx.rtv)      { ctx.rtv->Release();      ctx.rtv = nullptr; }
    if (ctx.swapChain){ ctx.swapChain->Release(); ctx.swapChain = nullptr; }
    if (ctx.deviceCtx){ ctx.deviceCtx->Release(); ctx.deviceCtx = nullptr; }
    if (ctx.device)   { ctx.device->Release();   ctx.device = nullptr; }
    if (ctx.hwnd)     { DestroyWindow(ctx.hwnd); ctx.hwnd = nullptr; }
}
