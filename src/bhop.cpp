// --- src/bhop.cpp ---
// Bhop via SendInput keyboard jump — no bind required.
// Detects landing frame via m_hGroundEntity, sends a SPACE keydown+keyup
// through the Win32 input stack on the landing tick.
#include "bhop.h"
#include "memory.h"
#include "offsets.h"
#include <Windows.h>
#include <thread>
#include <chrono>

constexpr uintptr_t m_hGroundEntity = 0x50C;

static void send_jump() {
    INPUT inputs[2]{};

    // SPACE down
    inputs[0].type       = INPUT_KEYBOARD;
    inputs[0].ki.wVk     = VK_SPACE;
    inputs[0].ki.dwFlags = 0;

    // SPACE up
    inputs[1].type       = INPUT_KEYBOARD;
    inputs[1].ki.wVk     = VK_SPACE;
    inputs[1].ki.dwFlags = KEYEVENTF_KEYUP;

    SendInput(2, inputs, sizeof(INPUT));
}

void BhopTick() {
    static bool wasInAir = false;

    // Only run when player is holding space
    if (!(GetAsyncKeyState(VK_SPACE) & 0x8000)) {
        wasInAir = false;
        return;
    }

    uintptr_t local_pawn = g_mem.read<uintptr_t>(
        g_mem.client_dll + offsets::dwLocalPlayerPawn);
    if (!local_pawn) return;

    uint32_t ground = g_mem.read<uint32_t>(local_pawn + m_hGroundEntity);
    bool inAir = (ground == 0xFFFFFFFF);

    if (wasInAir && !inAir) {
        // Just landed — send jump immediately
        send_jump();
    }

    wasInAir = inAir;
}
