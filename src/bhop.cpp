// --- src/bhop.cpp ---
// Bhop via CS2 button system write.
// PRESS = 65537, RELEASE = 256  (every currently-working bhop uses these;
// writing 0 for release leaves the button state machine desynced).
// JUMP BUTTON: 0xB3E00 (verified working for current build - DO NOT change)
#include "bhop.h"
#include "memory.h"
#include "offsets.h"
#include <Windows.h>

constexpr uintptr_t dwJumpButton    = 0xB3E00; // client.dll buttons::jump
constexpr uintptr_t m_hGroundEntity = 0x50C;   // C_BaseEntity::m_hGroundEntity

constexpr uint64_t kPress   = 65537; // button down
constexpr uint64_t kRelease = 256;   // explicit button release

void Bhop_Init() {
    // Clear any leftover button state once at startup.
    if (g_mem.is_valid())
        g_mem.write<uint64_t>(g_mem.client_dll + dwJumpButton, kRelease);
}

void BhopTick() {
    if (!g_mem.is_valid()) return;

    // Space released -> send one release and stop touching it this tick.
    if (!(GetAsyncKeyState(VK_SPACE) & 0x8000)) {
        g_mem.write<uint64_t>(g_mem.client_dll + dwJumpButton, kRelease);
        return;
    }

    const uintptr_t local_pawn =
        g_mem.read<uintptr_t>(g_mem.client_dll + offsets::dwLocalPlayerPawn);
    if (!local_pawn) {
        g_mem.write<uint64_t>(g_mem.client_dll + dwJumpButton, kRelease);
        return;
    }

    // 0xFFFFFFFF = no ground entity -> airborne.
    const bool in_air =
        g_mem.read<uint32_t>(local_pawn + m_hGroundEntity) == 0xFFFFFFFFu;

    // On ground: press jump (re-jumps on every landing). In air: release.
    g_mem.write<uint64_t>(g_mem.client_dll + dwJumpButton,
                          in_air ? kRelease : kPress);
}
