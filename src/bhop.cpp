// --- src/bhop.cpp ---
// Bhop via CS2 button system write - 2026 Optimized
// JUMP BUTTON: 0xB3E00 (DO NOT change - verified working for current build)
#include "bhop.h"
#include "memory.h"
#include "offsets.h"
#include <Windows.h>

constexpr uintptr_t dwJumpButton    = 0xB3E00; // client.dll buttons::jump
constexpr uintptr_t m_hGroundEntity = 0x50C;   // C_BaseEntity::m_hGroundEntity

void Bhop_Init() {
    // Nothing to set up; BhopTick() runs from the memory thread at 4 ms.
}

void BhopTick() {
    // Space released -> make sure jump is clear and do nothing else.
    if (!(GetAsyncKeyState(VK_SPACE) & 0x8000)) {
        if (g_mem.is_valid())
            g_mem.write<uint64_t>(g_mem.client_dll + dwJumpButton, 0ULL);
        return;
    }

    if (!g_mem.is_valid()) return;

    const uintptr_t local_pawn =
        g_mem.read<uintptr_t>(g_mem.client_dll + offsets::dwLocalPlayerPawn);
    if (!local_pawn) return;

    // 0xFFFFFFFF = no ground entity -> in the air.
    const bool in_air =
        g_mem.read<uint32_t>(local_pawn + m_hGroundEntity) == 0xFFFFFFFFu;

    if (in_air) {
        // Airborne -> release so jump isn't held through the landing tick.
        g_mem.write<uint64_t>(g_mem.client_dll + dwJumpButton, 0ULL);
    } else {
        // On the ground -> press jump: re-jump on every landing.
        g_mem.write<uint64_t>(g_mem.client_dll + dwJumpButton, 65537ULL);
    }
}
