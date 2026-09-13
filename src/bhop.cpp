// --- src/bhop.cpp ---
// Bhop via CS2 button system write.
// The jump button is a CCSGOInputButton struct in client.dll.
// Writing buttonState = 3 (held) triggers the jump on landing.
// Offset from your September 11 build — re-dump if CS2 updates.
#include "bhop.h"
#include "memory.h"
#include "offsets.h"
#include <Windows.h>

// CCSGOInputButton layout:
//   +0x00: uint64 buttonFlags  (current held state)
//   +0x08: uint64 buttonState  (write 3 = pressed this tick)
//   +0x10: uint64 buttonState2
// Writing 65537 (0x10001) to buttonFlags triggers jump in CS2's input system
constexpr uintptr_t dwJumpButton    = 0x1813610; // client.dll buttons::jump (Sept 2026 build)
constexpr uintptr_t m_hGroundEntity = 0x50C;

void BhopTick() {
    static bool wasInAir = false;

    if (!(GetAsyncKeyState(VK_SPACE) & 0x8000)) {
        wasInAir = false;
        return;
    }

    uintptr_t local_pawn = g_mem.read<uintptr_t>(
        g_mem.client_dll + offsets::dwLocalPlayerPawn);
    if (!local_pawn) return;

    uint32_t ground = g_mem.read<uint32_t>(local_pawn + m_hGroundEntity);
    bool inAir = (ground == 0xFFFFFFFF);

    if (!inAir) {
        // On ground holding space — force jump button state
        g_mem.write<uint64_t>(g_mem.client_dll + dwJumpButton, 65537ULL);
    } else {
        // In air — clear it so we don't double-jump
        g_mem.write<uint64_t>(g_mem.client_dll + dwJumpButton, 0ULL);
    }

    wasInAir = inAir;
}
