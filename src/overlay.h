#pragma once
#include <d3d11.h>
#include <Windows.h>
#include <functional>

struct OverlayContext {
    HWND hwnd = nullptr;
    ID3D11Device* device = nullptr;
    ID3D11DeviceContext* deviceCtx = nullptr;
    IDXGISwapChain* swapChain = nullptr;
    ID3D11RenderTargetView* rtv = nullptr;
    int width = 1920;
    int height = 1080;
    bool running = true;
};

bool OverlayCreate(OverlayContext& ctx);
void OverlayDestroy(OverlayContext& ctx);
void OverlayRun(OverlayContext& ctx, std::function<void()> renderFn);
