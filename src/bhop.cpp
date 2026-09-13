// --- src/bhop.cpp ---
// Bhop via CS2 button system write - 2026 Optimized
// JUMP BUTTON: 0xB3E00 (Do NOT change - verified working for current build)
#include "bhop.h"
#include "memory.h"
#include "offsets.h"
#include <Windows.h>

constexpr uintptr_t dwJumpButton = 0xB3E00; // client.dll buttons::jump
constexpr uintptr_t m_hGroundEntity = 0x50C;

void Bhop_Init() {
    BhopTick(); // Initialize thread
}

void BhopTick() {
    static bool wasInAir = false;

    if (!(GetAsyncKeyState(VK_SPACE) & 0x8000)) {
        // Space released — clear button when space released
        if (g_mem.is_valid()) {
            g_mem.write<uint64_t>(g_mem.client_dll + dwJumpButton, 0ULL);
        }
        wasInAir = false;
        return;
    }

    uintptr_t local_pawn = g_mem.read<uintptr_t>(
        g_mem.client_dll + offsets::dwLocalPlayerPawn);
    if (!local_pawn) return;

    uint32_t ground = g_mem.read<uint32_t>(
        local_pawn + m_hGroundEntity);
    bool inAir = (ground == 0xFFFFFFFF);

    if (!inAir) {
        // On ground — write jump
        if (g_mem.is_valid()) {
            g_mem.write<uint64_t>(g_mem.client_dll + dwJumpButton, 65537ULL);
        }
    } else {
        // In air — clear so we don't hold jump
        if (g_mem.is_valid()) {
            g_mem.write<uint64_t>(g_mem.client_dll + dwJumpButton, 0ULL);
        }
    }

    wasInAir = inAir;
}
