// --- src/bhop.cpp ---
// Bhop via a direct write to the CS2 jump button.
//
//   +jump = 65537 (0x10001)   -jump = 256 (0x00000100)
//
// Only the ADDRESS moves, and it moves on every update -- so it is found at
// runtime rather than hard-coded. The button block is 13 dwords on a 0x90
// stride, which is what makes it findable without a fresh dump:
//
//   sprint -0x630   reload -0x5A0   attack -0x510   attack2 -0x480
//   turnleft -0x3F0 turnright -0x360 forward -0x2D0  back -0x240
//   left -0x1B0     right -0x120     use -0x090      JUMP   duck +0x090
//
// Unpressed button slots read EXACTLY 0 or 256, which is a very tight filter:
// a random region of memory essentially never satisfies it for all 13 slots.
#include "bhop.h"
#include "memory.h"
#include "offsets.h"

#include <Windows.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

extern HWND g_cs2_hwnd;

namespace {

constexpr int32_t kJumpPress   = 65537; // +jump
constexpr int32_t kJumpRelease = 256;   // -jump

constexpr intptr_t kRel[] = {
    -0x630, -0x5A0, -0x510, -0x480, -0x3F0, -0x360, -0x2D0,
    -0x240, -0x1B0, -0x120, -0x090,  0x000,  0x090,
};
constexpr int kSlotCount = static_cast<int>(sizeof(kRel) / sizeof(kRel[0]));
constexpr int kJumpSlot  = 11;

std::atomic<bool>      g_stop{false}, g_started{false}, g_locked{false};
std::atomic<bool>      g_scanning{false}, g_input_mode{false};
std::atomic<uintptr_t> g_jump_offset{offsets::dwForceJump};
std::atomic<bool>      g_dbg_ground{false};
std::atomic<bool>      g_dbg_focused{false};
std::atomic<int>       g_dbg_signals{0};

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

bool key_down(int vk) {
    return (GetAsyncKeyState(vk) & 0x8000) != 0;
}

// Which button slots the user is physically holding, as a bitmask over kRel.
int held_mask() {
    int m = 0;
    if (key_down(VK_SHIFT))   m |= 1 << 0;   // sprint
    if (key_down('R'))        m |= 1 << 1;   // reload
    if (key_down(VK_LBUTTON)) m |= 1 << 2;   // attack
    if (key_down(VK_RBUTTON)) m |= 1 << 3;   // attack2
    // slots 4/5 (turnleft/turnright) are usually unbound -> left unchecked
    if (key_down('W'))        m |= 1 << 6;
    if (key_down('S'))        m |= 1 << 7;
    if (key_down('A'))        m |= 1 << 8;
    if (key_down('D'))        m |= 1 << 9;
    if (key_down('E'))        m |= 1 << 10;  // use
    if (key_down(VK_SPACE))   m |= 1 << 11;  // jump
    if (key_down(VK_CONTROL)) m |= 1 << 12;  // duck
    return m;
}

// Is the 13-slot block anchored at j the button block?
//
//  * every slot the user IS holding must have its down bit set
//  * every slot the user is NOT holding must read exactly 0 or 256
//
// The second rule is what stops the scanner from locking onto `forward` when
// you hold W: from `forward`'s address the slot that would be "jump" reads
// 0x10001, which is neither 0 nor 256, so it is rejected.
bool block_valid(uintptr_t j, int mask, bool require_mixed) {
    int pressed = 0, released = 0;
    for (int i = 0; i < kSlotCount; ++i) {
        const uint32_t v = g_mem.read<uint32_t>(j + kRel[i]);

        if (i == 4 || i == 5) {   // turnleft / turnright: unbound, accept anything
            if (v != 0u && v != 0x100u && v != 0x10000u &&
                v != 0x101u && v != 0x10001u) return false;
            continue;
        }

        if ((mask >> i) & 1) {
            if ((v & 1u) == 0u) return false;         // must be held
            ++pressed;
        } else {
            if (v != 0u && v != 0x100u) return false; // must be idle
            ++released;
        }
    }
    // Without a key held we need at least one latent 256, otherwise a long run
    // of zeros would match every 0x90 stride inside it.
    if (require_mixed && pressed == 0 && released == kSlotCount) {
        bool any_256 = false;
        for (int i = 0; i < kSlotCount; ++i)
            if (g_mem.read<uint32_t>(j + kRel[i]) == 0x100u) any_256 = true;
        if (!any_256) return false;
    }
    return true;
}

// Sweep the window for dwords equal to 0x10001, then check the block.
uintptr_t scan_pressed(int mask) {
    const uintptr_t lo = g_mem.client_dll + offsets::kJumpScanLo;
    const uintptr_t hi = g_mem.client_dll + offsets::kJumpScanHi;

    static std::vector<uint8_t> buf;
    const size_t piece = 0x10000;
    if (buf.size() < piece) buf.resize(piece);

    for (uintptr_t a = lo; a + piece <= hi; a += piece) {
        if (g_stop.load()) return 0;
        if (!g_mem.read_bytes(a, buf.data(), piece)) continue;

        for (size_t k = 0; k + 4 <= piece; k += 4) {
            uint32_t v = 0;
            std::memcpy(&v, buf.data() + k, 4);
            if (v != 0x10001u) continue;

            const uintptr_t cand = a + k;
            if (held_mask() != mask) return 0;      // keys changed, retry later
            if (!block_valid(cand, mask, false)) continue;

            wait_ms(25);
            if (held_mask() != mask) return 0;      // keys changed, retry later
            if (!block_valid(cand, mask, false)) continue;
            return cand;
        }
    }
    return 0;
}

// Sweep for the block with nothing held (all slots 0 or 256).
uintptr_t scan_idle() {
    const uintptr_t lo = g_mem.client_dll + offsets::kJumpScanLo;
    const uintptr_t hi = g_mem.client_dll + offsets::kJumpScanHi;

    static std::vector<uint8_t> buf;
    const size_t piece = 0x10000;
    if (buf.size() < piece) buf.resize(piece);

    uintptr_t found = 0;
    int hits = 0;

    for (uintptr_t a = lo; a + piece <= hi; a += piece) {
        if (g_stop.load()) return 0;
        if (!g_mem.read_bytes(a, buf.data(), piece)) continue;

        for (size_t k = 0; k + 4 <= piece; k += 4) {
            uint32_t v = 0;
            std::memcpy(&v, buf.data() + k, 4);
            if (v != 0u && v != 0x100u) continue;

            const uintptr_t cand = a + k;
            const uintptr_t anchor =
                cand - static_cast<uintptr_t>(kRel[0]);  // candidate jump addr
            if (anchor < g_mem.client_dll) continue;
            if (!block_valid(anchor, 0, true)) continue;

            if (found != anchor) { ++hits; found = anchor; }
            if (hits > 3) return 0;   // too many matches, not distinctive
        }
    }
    return (hits == 1) ? found : 0;
}

void scan_thread_main() {
    for (int i = 0; i < 200 && !g_stop.load(); ++i) wait_ms(10);

    while (!g_stop.load()) {
        if (g_locked.load()) { wait_ms(500); continue; }

        g_scanning.store(true);

        uintptr_t hit = 0;
        const int mask = held_mask();
        if (mask != 0) hit = scan_pressed(mask);
        if (!hit)      hit = scan_idle();

        if (hit) {
            g_jump_offset.store(hit - g_mem.client_dll);
            g_locked.store(true);
        }
        g_scanning.store(false);

        if (!g_locked.load()) wait_ms(300);
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

        const bool focused = cs2_focused();
        g_dbg_focused.store(focused);

        if (!focused) {
            g_mem.write<int32_t>(jump_addr, kJumpRelease);
            continue;
        }
        if (!key_down(VK_SPACE)) {
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
        // settled before the game reads input for the next tick.
        if (now - last_sample >= 2.0) {
            last_sample = now;
            ground = sample_ground(g_mem, pawn);
        }

        // On the ground -> press, which re-jumps on every landing. Airborne ->
        // release, so the landing is always a fresh press edge.
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
    d.offset    = g_jump_offset.load();
    d.on_ground = g_dbg_ground.load();
    d.focused   = g_dbg_focused.load();
    d.signals   = g_dbg_signals.load();
    d.locked    = g_locked.load();
    d.scanning  = g_scanning.load();
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
