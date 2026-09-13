// --- src/bhop.cpp ---
// Bhop via a direct write to the CS2 jump button.
//
//   +jump = 65537 (0x10001)
//   -jump = 256   (0x00000100)
//
// Both have been stable for the whole life of CS2. Only the ADDRESS moves, and
// it moves on every update -- so this file never trusts a hard-coded offset.
// Instead the jump button is discovered at runtime by watching the button block
// while the user plays normally (strafing or holding SPACE).
//
// The button address is a VALUE, not a pointer, and the write is 4 bytes --
// writing 8 smears into the neighbouring button state.
//
// Timing note: this runs on its own ~1 ms thread with a spin-wait. Sleeping
// with sleep_for(1ms) on Windows actually sleeps ~15.6 ms because of the
// default timer granularity, which is why the jump kept missing the landing
// tick when it shared the 4 ms reader thread.
#include "bhop.h"
#include "memory.h"
#include "offsets.h"

#include <Windows.h>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <thread>

extern HWND g_cs2_hwnd;
extern bool g_menu_open;

namespace {

constexpr int32_t kJumpPress   = 65537; // +jump
constexpr int32_t kJumpRelease = 256;   // -jump

// The 13 CS2 buttons sit on a fixed 0x90 stride in a fixed order. These are
// the distances from `jump` to each of them (a2x's buttons dump). The LAYOUT
// is stable even though the base address isn't, which is what makes the block
// findable without knowing any offset.
constexpr intptr_t kButtonRel[] = {
    -0x630, // 0  sprint
    -0x5A0, // 1  reload
    -0x510, // 2  attack
    -0x480, // 3  attack2
    -0x3F0, // 4  turnleft
    -0x360, // 5  turnright
    -0x2D0, // 6  forward   W
    -0x240, // 7  back      S
    -0x1B0, // 8  left      A
    -0x120, // 9  right     D
    -0x090, // 10 use
     0x000, // 11 jump      SPACE
     0x090, // 12 duck      CTRL
};
constexpr size_t kButtonCount = sizeof(kButtonRel) / sizeof(kButtonRel[0]);
constexpr int    kJumpSlot    = 11;

std::atomic<bool>      g_stop{false};
std::atomic<bool>      g_started{false};
std::atomic<bool>      g_locked{false};
std::atomic<bool>      g_scanning{false};
std::atomic<bool>      g_input_mode{false};
std::atomic<uintptr_t> g_jump_offset{offsets::dwForceJump};

// Diagnostics for the menu.
std::atomic<uint32_t> g_dbg_flags{0};
std::atomic<uint32_t> g_dbg_hge{0};
std::atomic<bool>     g_dbg_ground{false};

std::thread g_bhop_thread;
std::thread g_scan_thread;

// ~1 ms sleep that actually works: sleep the bulk, spin the last stretch.
void wait_ms(int ms) {
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(ms);
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return;
        const auto left = deadline - now;
        if (left > std::chrono::milliseconds(2))
            std::this_thread::sleep_for(left - std::chrono::milliseconds(1));
        else
            std::this_thread::yield();
    }
}

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

// Which slots should be lit, given the keys the user is physically holding?
// Returned as a bitmask over kButtonRel indices. 0 means "nothing held", in
// which case there isn't enough evidence to identify the block.
int held_slot_mask() {
    int mask = 0;
    if (GetAsyncKeyState('W') & 0x8000)     mask |= 1 << 6;
    if (GetAsyncKeyState('S') & 0x8000)     mask |= 1 << 7;
    if (GetAsyncKeyState('A') & 0x8000)     mask |= 1 << 8;
    if (GetAsyncKeyState('D') & 0x8000)     mask |= 1 << 9;
    if (GetAsyncKeyState(VK_SPACE) & 0x8000) mask |= 1 << kJumpSlot;
    return mask;
}

bool block_ok(const Memory& mem, uintptr_t jump_addr, int mask) {
    for (size_t i = 0; i < kButtonCount; ++i) {
        const uint32_t v = mem.read<uint32_t>(
            jump_addr + static_cast<uintptr_t>(kButtonRel[i]));

        if (!plausible_button(v)) return false;

        // Every key the user is actually holding must light its own slot.
        if ((mask & (1 << i)) && (v & 1u) == 0u) return false;
    }

    // If SPACE isn't held, the jump slot can't be reading as pressed.
    if (!(mask & (1 << kJumpSlot))) {
        if (mem.read<uint32_t>(jump_addr) & 1u) return false;
    }
    return true;
}

// Sweep the window around the estimate. Cheap to abort: it just stops when the
// user releases every key.
bool sweep(int mask) {
    const uintptr_t center = g_mem.client_dll + offsets::dwForceJump;
    const uintptr_t lo = center - offsets::kJumpScanRadius;
    const uintptr_t hi = center + offsets::kJumpScanRadius;

    for (uintptr_t a = lo; a <= hi; a += offsets::kScanStep) {
        if (g_stop.load()) return false;
        if (held_slot_mask() == 0) return false;  // evidence gone, retry later
        if (mask != held_slot_mask()) return false;

        if (!block_ok(g_mem, a, mask)) continue;

        // Confirmed twice, ~25 ms apart, so a transient coincidence can't lock
        // us onto the wrong block.
        const uintptr_t candidate = a;
        wait_ms(25);
        if (!block_ok(g_mem, candidate, held_slot_mask())) continue;

        g_jump_offset.store(candidate - g_mem.client_dll);
        return true;
    }
    return false;
}

void scan_thread_main() {
    // Give the game time to finish loading.
    for (int i = 0; i < 200 && !g_stop.load(); ++i) wait_ms(10);

    while (!g_stop.load()) {
        if (g_locked.load()) { wait_ms(500); continue; }

        const int mask = held_slot_mask();
        if (mask == 0) { wait_ms(50); continue; }

        g_scanning.store(true);
        if (sweep(mask)) g_locked.store(true);
        g_scanning.store(false);

        if (!g_locked.load()) wait_ms(200);
    }
}

// ── ground detection ──────────────────────────────────────────────────────
// Two independent signals, so it still works on any surface and at any height
// (a landing from a big drop looks exactly like a landing on the flat):
//   m_fFlags          FL_ONGROUND bit
//   m_hGroundEntity   0x8000 while standing on something, 0xFFFFFFFF in air
bool read_ground(uintptr_t pawn, uint32_t& flags_out, uint32_t& hge_out) {
    flags_out = g_mem.read<uint32_t>(pawn + offsets::m_fFlags);
    hge_out   = g_mem.read<uint32_t>(pawn + offsets::m_hGroundEntity);

    const bool by_flags = (flags_out & 1u) != 0u;

    // Only trust m_hGroundEntity if it reads as one of its real values, so a
    // stale offset can't fake a ground contact.
    const bool hge_trusted = (hge_out == 0x8000u || hge_out == 0xFFFFFFFFu ||
                              hge_out == 0u);
    const bool by_ent = hge_trusted && (hge_out == 0x8000u);

    return by_flags || by_ent;
}

void bhop_thread_main() {
    // Park the button released so a stale pressed value can't wedge the jump.
    if (g_mem.is_valid())
        g_mem.write<int32_t>(g_mem.client_dll + g_jump_offset.load(), kJumpRelease);

    while (!g_stop.load()) {
        wait_ms(1);

        if (g_input_mode.load()) continue;
        if (!g_mem.is_valid()) continue;

        const uintptr_t jump_addr = g_mem.client_dll + g_jump_offset.load();

        if (g_menu_open || !cs2_focused()) {
            g_mem.write<int32_t>(jump_addr, kJumpRelease);
            continue;
        }

        if (!(GetAsyncKeyState(VK_SPACE) & 0x8000)) {
            g_mem.write<int32_t>(jump_addr, kJumpRelease);
            continue;
        }

        const uintptr_t pawn =
            g_mem.read<uintptr_t>(g_mem.client_dll + offsets::dwLocalPlayerPawn);
        if (!pawn) {
            g_mem.write<int32_t>(jump_addr, kJumpRelease);
            continue;
        }

        uint32_t flags = 0, hge = 0;
        const bool ground = read_ground(pawn, flags, hge);
        g_dbg_flags.store(flags);
        g_dbg_hge.store(hge);
        g_dbg_ground.store(ground);

        // On the ground -> press, which re-jumps on every landing. In the air
        // -> release, so the next landing is always a fresh press edge.
        g_mem.write<int32_t>(jump_addr, ground ? kJumpPress : kJumpRelease);
    }
}

} // namespace

void Bhop_Init() {
    if (!g_mem.is_valid()) return;
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;

    g_stop.store(false);
    g_bhop_thread = std::thread(bhop_thread_main);
    g_scan_thread = std::thread(scan_thread_main);
}

void Bhop_Shutdown() {
    g_stop.store(true);
    if (g_bhop_thread.joinable()) g_bhop_thread.join();
    if (g_scan_thread.joinable()) g_scan_thread.join();
    g_started.store(false);
}

BhopDebug Bhop_GetDebug() {
    BhopDebug d;
    d.offset        = g_jump_offset.load();
    d.flags         = g_dbg_flags.load();
    d.ground_entity = g_dbg_hge.load();
    d.on_ground     = g_dbg_ground.load();
    d.locked        = g_locked.load();
    d.scanning      = g_scanning.load();
    if (g_mem.is_valid())
        d.live_value = g_mem.read<uint32_t>(g_mem.client_dll + d.offset);
    return d;
}

void Bhop_Rescan() {
    g_locked.store(false);
    if (g_scan_thread.joinable()) g_scan_thread.join();
    if (!g_started.load()) return;
    g_scan_thread = std::thread(scan_thread_main);
}

bool Bhop_InputMode() { return g_input_mode.load(); }

void Bhop_SetInputMode(bool enabled) {
    g_input_mode.store(enabled);
}
