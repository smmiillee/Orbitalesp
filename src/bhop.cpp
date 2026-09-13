// --- src/bhop.cpp ---
// Bhop via CS2 button system write.
// jump button offset from a2x/cs2-dumper buttons.hpp 2026-08-29
#include "bhop.h"
#include "memory.h"
#include "offsets.h"
#include <Windows.h>

constexpr uintptr_t dwJumpButton    = 0x20B3E00; // client.dll buttons::jump
constexpr uintptr_t m_hGroundEntity = 0x50C;

void BhopTick() {
    static bool wasInAir = false;

    if (!(GetAsyncKeyState(VK_SPACE) & 0x8000)) {
        wasInAir = false;
        // Clear button when space released
        g_mem.write<uint64_t>(g_mem.client_dll + dwJumpButton, 0ULL);
        return;
    }

    uintptr_t local_pawn = g_mem.read<uintptr_t>(
        g_mem.client_dll + offsets::dwLocalPlayerPawn);
    if (!local_pawn) return;

    uint32_t ground = g_mem.read<uint32_t>(local_pawn + m_hGroundEntity);
    bool inAir = (ground == 0xFFFFFFFF);

    if (!inAir) {
        // On ground holding space — write jump
        g_mem.write<uint64_t>(g_mem.client_dll + dwJumpButton, 65537ULL);
    } else {
        // In air — clear so we don't hold jump
        g_mem.write<uint64_t>(g_mem.client_dll + dwJumpButton, 0ULL);
    }

    wasInAir = inAir;
}
