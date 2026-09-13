// --- src/bhop.cpp ---
#include "bhop.h"
#include "memory.h"
#include "offsets.h"
#include <Windows.h>
#include <thread>
#include <chrono>

static constexpr uintptr_t m_hGroundEntity = 0x50C;

static void SendScrollDown() {
    INPUT inp{};
    inp.type         = INPUT_MOUSE;
    inp.mi.dwFlags   = MOUSEEVENTF_WHEEL;
    inp.mi.mouseData = static_cast<DWORD>(-WHEEL_DELTA);
    SendInput(1, &inp, sizeof(INPUT));
}

void BhopTick() {
    static uint32_t lastGroundEntity = 0xFFFFFFFF;
    static bool     wasInAir         = false;

    if (!(GetAsyncKeyState(VK_SPACE) & 0x8000)) {
        lastGroundEntity = 0xFFFFFFFF;
        wasInAir         = false;
        return;
    }

    HWND cs2Hwnd = FindWindowA("SDL_app", nullptr);
    if (!cs2Hwnd || GetForegroundWindow() != cs2Hwnd)
        return;

    // Use client_dll base — dwLocalPlayerPawn is a client.dll offset
    uintptr_t localPawn = g_mem.read<uintptr_t>(
        g_mem.client_dll + offsets::dwLocalPlayerPawn);
    if (!localPawn) return;

    uint32_t groundEnt = g_mem.read<uint32_t>(localPawn + m_hGroundEntity);
    bool inAir = (groundEnt == 0xFFFFFFFF);

    if (wasInAir && !inAir) {
        SendScrollDown();
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        SendScrollDown();
    }

    wasInAir         = inAir;
    lastGroundEntity = groundEnt;
}
