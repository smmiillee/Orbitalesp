// --- src/bhop.cpp ---
#include "bhop.h"
#include "memory.h"
#include "offsets.h"

#include <Windows.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <thread>

extern HWND g_cs2_hwnd;

namespace {

constexpr int32_t kJumpPress   = 65537; // +jump
constexpr int32_t kJumpRelease = 256;   // -jump

// The 13 CS2 buttons sit on a fixed 0x90 stride in a fixed order; the LAYOUT is
// stable even though the base address is not.
constexpr intptr_t kButtonRel[] = {
    -0x630, -0x5A0, -0x510, -0x480, -0x3F0, -0x360, -0x2D0,
    -0x240, -0x1B0, -0x120, -0x090,  0x000,  0x090,
};
constexpr size_t kButtonCount = sizeof(kButtonRel) / sizeof(kButtonRel[0]);
constexpr int    kJumpSlot    = 11;

std::atomic<bool>      g_stop{false}, g_started{false}, g_locked{false};
std::atomic<bool>      g_scanning{false}, g_input_mode{false};
std::atomic<uintptr_t> g_jump_offset{offsets::dwForceJump};
std::atomic<bool>      g_dbg_ground{false};
std::atomic<bool>      g_dbg_focused{false};
std::atomic<int>       g_dbg_signals{0};
std::atomic<int>       g_dbg_calib{0};

std::thread g_bhop_thread;
std::thread g_scan_thread;

double now_ms() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0).count();
}

// ~1 ms sleep that actually works: sleep the bulk, spin the last stretch.
// Plain sleep_for(1ms) on Windows really sleeps ~15.6 ms -- a whole game tick,
// which is enough on its own to make bhop miss landings.
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
    return g_cs2_hwnd && GetForegroundWindow() == g_cs2_hwnd;
}

// ── jump-button scanner ──────────────────────────────────────────────────

bool plausible_button(uint32_t v) {
    return v == 0x0u || v == 0x1u || v == 0x100u
        || v == 0x101u || v == 0x10000u || v == 0x10001u;
}

int held_slot_mask() {
    int mask = 0;
    if (GetAsyncKeyState('W') & 0x8000)      mask |= 1 << 6;
    if (GetAsyncKeyState('S') & 0x8000)      mask |= 1 << 7;
    if (GetAsyncKeyState('A') & 0x8000)      mask |= 1 << 8;
    if (GetAsyncKeyState('D') & 0x8000)      mask |= 1 << 9;
    if (GetAsyncKeyState(VK_SPACE) & 0x8000) mask |= 1 << kJumpSlot;
    return mask;
}

bool block_ok(const Memory& mem, uintptr_t jump_addr, int mask) {
    for (size_t i = 0; i < kButtonCount; ++i) {
        const uint32_t v = mem.read<uint32_t>(
            jump_addr + static_cast<uintptr_t>(kButtonRel[i]));
        if (!plausible_button(v)) return false;
        if ((mask & (1 << i)) && (v & 1u) == 0u) return false;
    }
    if (!(mask & (1 << kJumpSlot)) && (mem.read<uint32_t>(jump_addr) & 1u))
        return false;
    return true;
}

bool sweep(int mask) {
    const uintptr_t center = g_mem.client_dll + offsets::dwForceJump;
    const uintptr_t lo = center - offsets::kJumpScanRadius;
    const uintptr_t hi = center + offsets::kJumpScanRadius;

    for (uintptr_t a = lo; a <= hi; a += offsets::kScanStep) {
        if (g_stop.load()) return false;
        if (held_slot_mask() == 0) return false;
        if (mask != held_slot_mask()) return false;
        if (!block_ok(g_mem, a, mask)) continue;

        wait_ms(25);
        if (!block_ok(g_mem, a, held_slot_mask())) continue;

        g_jump_offset.store(a - g_mem.client_dll);
        return true;
    }
    return false;
}

void scan_thread_main() {
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

// ── ground signals, graded against the local player's Z ──────────────────

struct GroundWatch {
    bool   have_z = false;
    float  z = 0.0f;
    double z_changed = 0.0;
    bool   z_ground = true;

    int    flag_g = 0, flag_a = 0;
    int    hge_g  = 0, hge_a  = 0;
    bool   flag_ok = false, hge_ok = false;
};
GroundWatch g_gw;

bool sample_ground(const Memory& mem, uintptr_t pawn) {
    const double now = now_ms();

    const float    z   = mem.read<float>(pawn + offsets::m_vOldOrigin + 8);
    const uint32_t fl  = mem.read<uint32_t>(pawn + offsets::m_fFlags);
    const uint32_t hge = mem.read<uint32_t>(pawn + offsets::m_hGroundEntity);

    if (!g_gw.have_z) { g_gw.have_z = true; g_gw.z = z; g_gw.z_changed = now; }
    if (std::fabs(z - g_gw.z) >= 0.5f) { g_gw.z = z; g_gw.z_changed = now; }
    g_gw.z_ground = (now - g_gw.z_changed) > 22.0;

    if (g_gw.z_ground) {
        if (fl & 1u)            ++g_gw.flag_g;
        if (hge != 0xFFFFFFFFu) ++g_gw.hge_g;
    } else {
        if (!(fl & 1u))          ++g_gw.flag_a;
        if (hge == 0xFFFFFFFFu)  ++g_gw.hge_a;
    }

    // Only trust a signal once it has been seen in BOTH states.
    if (!g_gw.flag_ok && g_gw.flag_g >= 6 && g_gw.flag_a >= 6) g_gw.flag_ok = true;
    if (!g_gw.hge_ok  && g_gw.hge_g  >= 6 && g_gw.hge_a  >= 6) g_gw.hge_ok  = true;

    int sig = 1;
    if (g_gw.flag_ok) sig |= 2;
    if (g_gw.hge_ok)  sig |= 4;
    g_dbg_signals.store(sig);
    g_dbg_calib.store(sig == 1 ? 40 : (sig == 3 || sig == 5 ? 75 : 100));

    const bool ground = g_gw.z_ground
                     || (g_gw.flag_ok && (fl & 1u))
                     || (g_gw.hge_ok  && hge != 0xFFFFFFFFu);
    g_dbg_ground.store(ground);
    return ground;
}

void bhop_thread_main() {
    if (g_mem.is_valid())
        g_mem.write<int32_t>(g_mem.client_dll + g_jump_offset.load(), kJumpRelease);

    uintptr_t pawn = 0;
    double    pawn_at = 0.0;
    double    last_sample = 0.0;
    bool      ground = false;

    while (!g_stop.load()) {
        wait_ms(1);
        if (g_input_mode.load()) continue;
        if (!g_mem.is_valid()) continue;

        const uintptr_t jump_addr = g_mem.client_dll + g_jump_offset.load();

        // Note: the menu no longer gates bhop. The overlay is non-activating,
        // so CS2 keeps keyboard focus even with the menu open -- which means
        // the panel can show live ground/signals while you play.
        const bool focused = cs2_focused();
        g_dbg_focused.store(focused);

        if (!focused) {
            g_mem.write<int32_t>(jump_addr, kJumpRelease);
            continue;
        }
        if (!(GetAsyncKeyState(VK_SPACE) & 0x8000)) {
            g_mem.write<int32_t>(jump_addr, kJumpRelease);
            continue;
        }

        const double now = now_ms();
        if (!pawn || now - pawn_at > 500.0) {
            pawn = g_mem.read<uintptr_t>(
                g_mem.client_dll + offsets::dwLocalPlayerPawn);
            pawn_at = now;
        }
        if (!pawn) {
            g_mem.write<int32_t>(jump_addr, kJumpRelease);
            continue;
        }

        // Sample at 2 ms but write every loop (~1 ms), so the button state is
        // already settled before the game reads input for the next tick.
        if (now - last_sample >= 2.0) {
            last_sample = now;
            ground = sample_ground(g_mem, pawn);
        }

        // On the ground -> press, which re-jumps on every landing. Airborne ->
        // release, so the next landing is always a fresh press edge.
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
    d.offset      = g_jump_offset.load();
    d.on_ground   = g_dbg_ground.load();
    d.focused     = g_dbg_focused.load();
    d.signals     = g_dbg_signals.load();
    d.calibrating = g_dbg_calib.load();
    d.locked      = g_locked.load();
    d.scanning    = g_scanning.load();
    return d;
}

void Bhop_Rescan() {
    g_locked.store(false);
    if (g_scan_thread.joinable()) g_scan_thread.join();
    if (!g_started.load()) return;
    g_scan_thread = std::thread(scan_thread_main);
}

bool Bhop_InputMode() { return g_input_mode.load(); }
void Bhop_SetInputMode(bool enabled) { g_input_mode.store(enabled); }
