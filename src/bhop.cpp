// --- src/bhop.cpp ---
// Bhop via dwForceJump memory write — most reliable external method.
// Reads m_hGroundEntity to detect landing, writes jump flag on landing frame.
#include "bhop.h"
#include "memory.h"
#include "offsets.h"
#include <Windows.h>

constexpr uintptr_t m_hGroundEntity = 0x50C;
constexpr uintptr_t dwForceJump     = 0x173E8E0; // client.dll offset

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
        // On ground while holding space — write jump
        g_mem.write<int>(g_mem.client_dll + dwForceJump, 65537);
    }

    wasInAir = inAir;
}
