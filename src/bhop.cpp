// --- src/bhop.cpp ---
// Bhop via a direct write to the CS2 jump button.
//
//   +jump = 65537 (0x10001)
//   -jump = 256   (0x00000100)
//
// Both values have been stable for the whole life of CS2; only the ADDRESS
// moves, and it moves on essentially every update. So it is not hard-coded:
// Bhop_DetectJump() finds the button block at runtime and Bhop_Init() uses
// whatever it resolved.
//
// The button address is a VALUE, not a pointer, and the write is 4 bytes --
// writing 8 smears into the neighbouring button state.
#include "bhop.h"
#include "memory.h"
#include "offsets.h"

#include <Windows.h>
#include <cstdint>
#include <cstddef>

extern HWND g_cs2_hwnd;
extern bool g_menu_open;

namespace {

constexpr int32_t kJumpPress   = 65537; // +jump
constexpr int32_t kJumpRelease = 256;   // -jump

// The 13 CS2 buttons sit on a fixed 0x90 stride in a fixed order. These are
// the distances from `jump` to each of them (a2x's buttons dump). Because the
// LAYOUT is stable even though the base address isn't, the block can be found
// by looking for this signature rather than by guessing an address.
constexpr intptr_t kButtonRel[] = {
    -0x630, // sprint
    -0x5A0, // reload
    -0x510, // attack
    -0x480, // attack2
    -0x3F0, // turnleft
    -0x360, // turnright
    -0x2D0, // forward    <- hold W for a strong signal
    -0x240, // back
    -0x1B0, // left
    -0x120, // right
    -0x090, // use
     0x000, // jump       <- hold SPACE
     0x090, // duck
};
constexpr size_t  kButtonCount  = sizeof(kButtonRel) / sizeof(kButtonRel[0]);
constexpr int     kJumpSlot     = 11;   // index of kButtonRel[] == 0
constexpr int     kForwardSlot  = 6;    // index of kButtonRel[] == -0x2D0

uintptr_t g_jump_offset = offsets::dwForceJump; // resolved at runtime
bool      g_input_mode  = false;

bool cs2_focused() {
    if (!g_cs2_hwnd) return false;
    return GetForegroundWindow() == g_cs2_hwnd;
}

// Every button state is 0 when idle, 0x100 after a release and 0x10001 while
// pressed. Anything else means this is not the button block.
bool plausible_button(uint32_t v) {
    return v == 0x0u || v == 0x1u || v == 0x100u
        || v == 0x101u || v == 0x10000u || v == 0x10001u;
}

// Does the 13-slot block anchored at `jump_addr` look like the button block?
// When `pressed_slot` >= 0, that slot must currently have its down-bit set.
bool looks_like_button_block(const Memory& mem, uintptr_t jump_addr,
                             int pressed_slot, int* zeros_out) {
    int zeros = 0;
    for (size_t i = 0; i < kButtonCount; ++i) {
        const uintptr_t a =
            jump_addr + static_cast<uintptr_t>(kButtonRel[i]);
        const uint32_t v = mem.read<uint32_t>(a);

        if (!plausible_button(v)) return false;
        if (v == 0u) ++zeros;
        if (static_cast<int>(i) == pressed_slot && (v & 1u) == 0u) return false;
    }
    if (zeros_out) *zeros_out = zeros;
    return true;
}

// ── input-mode fallback ───────────────────────────────────────────────────
// Toggle a synthetic spacebar so the game sees a fresh press every few ms.
// No offsets, so nothing here can go stale.
void inject_space(bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = VK_SPACE;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_SPACE, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = down ? 0u : KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(INPUT));
}

void input_mode_tick() {
    static bool phase = false;
    static ULONGLONG next_toggle = 0;

    if (!(GetAsyncKeyState(VK_SPACE) & 0x8000)) {
        if (phase) { inject_space(false); phase = false; }
        next_toggle = 0;
        return;
    }

    const ULONGLONG now = GetTickCount64();
    if (now < next_toggle) return;
    next_toggle = now + 8; // 8 ms half-period -> the game tick always samples it

    phase = !phase;
    inject_space(phase);
}

} // namespace

// ── public API ────────────────────────────────────────────────────────────

uintptr_t Bhop_JumpOffset()   { return g_jump_offset; }
void      Bhop_SetJumpOffset(uintptr_t off) { g_jump_offset = off; }

uint32_t Bhop_LiveJumpValue() {
    if (!g_mem.is_valid()) return 0;
    return g_mem.read<uint32_t>(g_mem.client_dll + g_jump_offset);
}

bool Bhop_InputMode() { return g_input_mode; }
void Bhop_SetInputMode(bool enabled) {
    g_input_mode = enabled;
    if (!enabled) inject_space(false);
}

bool Bhop_IsOnGround() {
    if (!g_mem.is_valid()) return false;
    const uintptr_t pawn =
        g_mem.read<uintptr_t>(g_mem.client_dll + offsets::dwLocalPlayerPawn);
    if (!pawn) return false;

    // FL_ONGROUND = (1 << 0).
    // (Alternative if this ever reads wrong: m_hGroundEntity != 0xFFFFFFFF.)
    const uint32_t flags = g_mem.read<uint32_t>(pawn + offsets::m_fFlags);
    return (flags & 1u) != 0u;
}

int Bhop_DetectJump() {
    if (!g_mem.is_valid()) return -1;

    const uintptr_t base = g_mem.client_dll + offsets::dwForceJump;
    const uintptr_t lo = base - offsets::kJumpScanRadius;
    const uintptr_t hi = base + offsets::kJumpScanRadius;

    // A physically held key lights exactly one slot, which is by far the
    // strongest signal we can get -- and it removes the ambiguity of an idle
    // block of zeros.
    int want_slot = -1;
    if (GetAsyncKeyState(VK_SPACE) & 0x8000) want_slot = kJumpSlot;
    else if (GetAsyncKeyState('W') & 0x8000) want_slot = kForwardSlot;

    if (want_slot >= 0) {
        for (uintptr_t a = lo; a <= hi; a += offsets::kScanStep) {
            if (looks_like_button_block(g_mem, a, want_slot, nullptr)) {
                g_jump_offset = a - g_mem.client_dll;
                return 1;
            }
        }
        return -1;
    }

    // Nothing held: structural only. Require at least one non-zero slot so we
    // can't lock onto a run of zeros, and require the jump slot itself to be
    // idle (it can never read as "pressed" while nobody is holding anything).
    uintptr_t best = 0;
    int best_zeros = -1;
    for (uintptr_t a = lo; a <= hi; a += offsets::kScanStep) {
        const uint32_t jump_val = g_mem.read<uint32_t>(a);
        if (jump_val != 0u && jump_val != 0x100u && jump_val != 0x101u) continue;

        int zeros = 0;
        if (!looks_like_button_block(g_mem, a, -1, &zeros)) continue;
        if (zeros == static_cast<int>(kButtonCount)) continue;

        if (zeros > best_zeros) { best_zeros = zeros; best = a; }
    }

    if (best) { g_jump_offset = best - g_mem.client_dll; return 0; }
    return -1;
}

void Bhop_Init() {
    // Park the button in a known released state so a stale pressed value from
    // a previous run can't wedge the jump.
    if (!g_mem.is_valid()) return;
    g_mem.write<int32_t>(g_mem.client_dll + g_jump_offset, kJumpRelease);
}

void BhopTick() {
    if (g_input_mode) { input_mode_tick(); return; }
    if (!g_mem.is_valid()) return;

    const uintptr_t jump_addr = g_mem.client_dll + g_jump_offset;

    // Never touch the button while the menu is open (the user may be typing)
    // or while CS2 is not the foreground window.
    if (g_menu_open || !cs2_focused()) {
        g_mem.write<int32_t>(jump_addr, kJumpRelease);
        return;
    }

    if (!(GetAsyncKeyState(VK_SPACE) & 0x8000)) {
        g_mem.write<int32_t>(jump_addr, kJumpRelease);
        return;
    }

    // On the ground -> press, which re-jumps on every landing. In the air ->
    // release, so the next landing is always a fresh press.
    g_mem.write<int32_t>(jump_addr, Bhop_IsOnGround() ? kJumpPress : kJumpRelease);
}
