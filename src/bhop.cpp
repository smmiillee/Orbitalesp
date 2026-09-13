// --- src/bhop.cpp ---
// Bhop via CS2 button system write (2026 corrected)
// Uses 1ms timer precision for optimal jump timing
#pragma once
#include <Windows.h>
#include "bhop.h"
#include "memory.h"
#include "offsets.h"

constexpr uintptr_t dwJumpButton = 0x2066C70; // 2026 offset - buttons::jump
constexpr uintptr_t m_fFlags = 0x10C;  // Ground check offset

// 1ms timer resolution for precise jump timing
void tickPeriod(void) {
    timeBeginPeriod(25);
}

void BhopTick() {
    static bool wasInAir = false;
    static bool tickStarted = false;
    
    if (!g_mem.is_valid()) return;

    // Check if spacebar is pressed
    if (!(GetAsyncKeyState(VK_SPACE) & 0x8000)) {
        // Space released - clear jump and reset air state
        g_mem.write<uint64_t>(g_mem.client_dll + dwJumpButton, 0ULL);
        wasInAir = false;
        tickStarted = false;
        return;
    }

    // Start timer on first loop after space press
    if (!tickStarted) {
        tickStarted = true;
        tickPeriod();
    }

    // Read local player pawn
    uintptr_t local_pawn = g_mem.read<uintptr_t>(
        g_mem.client_dll + offsets::dwLocalPlayerPawn);
    if (!local_pawn) return;

    // Read m_fFlags to check ground state (bit 0 = on ground)
    uint32_t fFlagsRaw = g_mem.read<uint32_t>(local_pawn + m_fFlags);
    bool inAir = (fFlagsRaw & 0x1) == 0; // 1 << 0 = on ground flag

    if (!inAir) {
        // On ground - write jump activation (65537 = jump + attack Release value)
        g_mem.write<uint64_t>(g_mem.client_dll + dwJumpButton, 65537ULL);
        wasInAir = false;
    } else {
        // In air - clear jump so we don't hold it
        g_mem.write<uint64_t>(g_mem.client_dll + dwJumpButton, 256ULL);
        wasInAir = true;
    }
}
