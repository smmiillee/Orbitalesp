// --- src/bhop.cpp ---
// Bhop via a direct write to the CS2 jump button.
//
//   +jump = 65537 (0x10001)
//   -jump = 256   (0x00000100)
// Both values have been stable across every CS2 build; only the offset moves.
// The write is 4 bytes -- writing 8 bytes smears into the adjacent button state.
//
// The button address is NOT a pointer. It is a value that sits directly in
// client.dll, so it is written as (client.dll + offsets::dwForceJump).
#include "bhop.h"
#include "memory.h"
#include "offsets.h"

#include <Windows.h>
#include <cstdint>

extern HWND g_cs2_hwnd;
extern bool g_menu_open;

namespace {

constexpr int32_t kJumpPress   = 65537; // +jump
constexpr int32_t kJumpRelease = 256;   // -jump

bool cs2_focused() {
    if (!g_cs2_hwnd) return false;
    return GetForegroundWindow() == g_cs2_hwnd;
}

} // namespace

void Bhop_Init() {
    // Park the button in a known released state once at startup, so a stale
    // pressed value from a previous run can't wedge the jump.
    if (!g_mem.is_valid()) return;
    g_mem.write<int32_t>(g_mem.client_dll + offsets::dwForceJump, kJumpRelease);
}

void BhopTick() {
    if (!g_mem.is_valid()) return;

    const uintptr_t jump_addr = g_mem.client_dll + offsets::dwForceJump;

    // Never touch the button while the menu is open (the user may be typing)
    // or while CS2 is not the foreground window.
    if (g_menu_open || !cs2_focused()) {
        g_mem.write<int32_t>(jump_addr, kJumpRelease);
        return;
    }

    // Not holding space -> guarantee the button is released, then bail.
    if (!(GetAsyncKeyState(VK_SPACE) & 0x8000)) {
        g_mem.write<int32_t>(jump_addr, kJumpRelease);
        return;
    }

    const uintptr_t local_pawn =
        g_mem.read<uintptr_t>(g_mem.client_dll + offsets::dwLocalPlayerPawn);
    if (!local_pawn) {
        g_mem.write<int32_t>(jump_addr, kJumpRelease);
        return;
    }

    // FL_ONGROUND = (1 << 0)
    const uint32_t flags = g_mem.read<uint32_t>(local_pawn + offsets::m_fFlags);
    const bool on_ground = (flags & 1u) != 0u;

    // On the ground -> press (re-jumps on every landing, which is the bhop).
    // In the air   -> release, so the next landing is a fresh press.
    //
    // If you'd rather use the ground-entity route instead of m_fFlags, swap
    // this for:
    //   const bool on_ground =
    //       g_mem.read<uint32_t>(local_pawn + offsets::m_hGroundEntity) != 0xFFFFFFFFu;
    g_mem.write<int32_t>(jump_addr, on_ground ? kJumpPress : kJumpRelease);
}
