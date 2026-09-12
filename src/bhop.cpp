#include "bhop.h"
#include "memory.h"
#include "offsets.h"
#include <Windows.h>
#include <thread>
#include <chrono>

// ─────────────────────────────────────────────────────────────────────────────
// Read-only subtick bhop via SendInput scroll wheel simulation.
//
// How it works:
//   1. Read m_hGroundEntity from local player pawn — PROCESS_VM_READ only.
//      0x0000FFFF = on ground. 0xFFFFFFFF = in air.
//   2. When SPACE is held AND we just transitioned from air→ground (landed),
//      fire a MOUSEEVENTF_WHEEL scroll-down event into the CS2 window.
//   3. CS2 receives the scroll event and triggers +jump via the player's
//      mwheeldown bind — which must be set in autoexec.cfg:
//          bind mwheeldown "+jump"
//
// Zero bytes written to game memory. No dwForceJump. No WPM.
// The input goes through the normal Win32 input stack — indistinguishable
// from a physical scroll wheel flick timed by a skilled player.
// ─────────────────────────────────────────────────────────────────────────────

// m_hGroundEntity offset within C_CSPlayerPawn
// On ground:  value & 0xFFFF == 0x7FFF  (handle points to world entity)
// In air:     value == 0xFFFFFFFF
static constexpr uintptr_t m_hGroundEntity = 0x50C;

static void SendScrollDown() {
    // Fires one WHEEL_DELTA scroll down into the foreground window.
    // CS2 must be focused — if our overlay is topmost/noactivate,
    // CS2 stays the foreground window during play.
    INPUT inp{};
    inp.type           = INPUT_MOUSE;
    inp.mi.dwFlags     = MOUSEEVENTF_WHEEL;
    inp.mi.mouseData   = static_cast<DWORD>(-WHEEL_DELTA); // scroll down
    SendInput(1, &inp, sizeof(INPUT));
}

void BhopTick() {
    static uint32_t lastGroundEntity = 0xFFFFFFFF;
    static bool     wasInAir         = false;

    // Only run when SPACE is held
    if (!(GetAsyncKeyState(VK_SPACE) & 0x8000)) {
        lastGroundEntity = 0xFFFFFFFF;
        wasInAir         = false;
        return;
    }

    // CS2 must be the active window — don't fire into nothing
    HWND cs2Hwnd = FindWindowA("SDL_app", nullptr);
    if (!cs2Hwnd || GetForegroundWindow() != cs2Hwnd)
        return;

    uintptr_t localPawn = g_Mem.Read<uintptr_t>(
        g_Mem.clientBase + offsets::dwLocalPlayerPawn);
    if (!localPawn) return;

    uint32_t groundEnt = g_Mem.Read<uint32_t>(localPawn + m_hGroundEntity);

    // In air: groundEnt is 0xFFFFFFFF
    // On ground: lower 15 bits are a valid entity index (world = 0)
    bool inAir = (groundEnt == 0xFFFFFFFF);

    // Detect landing frame: was in air, now on ground
    if (wasInAir && !inAir) {
        // Landed this tick — fire the scroll jump immediately
        SendScrollDown();

        // Second scroll 8ms later covers subtick landing jitter.
        // CS2 subtick samples input between ticks; the double-tap
        // ensures at least one scroll lands in the correct subtick window.
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
        SendScrollDown();
    }

    wasInAir         = inAir;
    lastGroundEntity = groundEnt;
}
