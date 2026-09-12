// --- src/bhop.cpp ---
#include "bhop.h"
#include "memory.h"   // brings in extern Memory g_mem + Memory::read<>()
#include "offsets.h"
#include <Windows.h>
#include <thread>
#include <chrono>

// ─────────────────────────────────────────────────────────────────────────────
// Read-only subtick bhop via SendInput scroll wheel simulation.
//
// How it works:
//   1. Read m_hGroundEntity from local player pawn — PROCESS_VM_READ only.
//      0xFFFFFFFF = in air. Anything else = on ground (world handle).
//   2. When SPACE is held AND we detect a landing frame (air -> ground),
//      fire MOUSEEVENTF_WHEEL scroll-down into the CS2 window.
//   3. CS2 receives the scroll and triggers +jump via mwheeldown bind.
//      Required in autoexec.cfg:
//          bind mwheeldown "+jump"
//
// Zero bytes written to game memory. No dwForceJump. No WPM.
// ─────────────────────────────────────────────────────────────────────────────

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

    // g_mem is defined in main.cpp, declared extern in memory.h
    uintptr_t localPawn = g_mem.read<uintptr_t>(
        g_mem.base_address + offsets::dwLocalPlayerPawn);
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
