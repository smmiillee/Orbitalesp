// --- src/bhop.cpp ---
// Bhop: either the CS2 jump button, or a synthesised spacebar.
//
//   +jump = 65537 (0x10001)   -jump = 256 (0x00000100)
//
// The button block is 13 dwords on a 0x90 stride:
//   sprint -0x630  reload -0x5A0  attack -0x510  attack2 -0x480
//   turnleft -0x3F0 turnright -0x360 forward -0x2D0 back -0x240
//   left -0x1B0     right -0x120     use -0x090      JUMP   duck +0x090
//
// INPUT MODE is the default because it is offset-free. It auto-jumps while
// grounded; it does NOT read the spacebar, precisely so its own injected
// keystrokes can't feed back into that test.
//
// MEMORY MODE never locks on a guess. A lock requires the candidate block to
// validate against a key you are physically holding, so a region of zeros can
// never masquerade as the button block.
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

// Only hints. A lock still has to be confirmed by a held key.
constexpr uintptr_t kCandidates[] = {
    0x2095490, 0x2096490, 0x2094490, 0x2093490, 0x205BAF0,
};

std::atomic<bool>      g_stop{false}, g_started{false}, g_locked{false};
std::atomic<bool>      g_scanning{false}, g_input_mode{true};
std::atomic<uintptr_t> g_jump_offset{offsets::dwForceJump};
std::atomic<bool>      g_dbg_ground{false};
std::atomic<bool>      g_dbg_focused{false};
std::atomic<int>       g_dbg_signals{0};
std::atomic<int>       g_dbg_cands{0};

std::thread g_bhop_thread, g_scan_thread;

double now_ms() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0).count();
}

// ~1 ms sleep that actually works: sleep the bulk, spin the last stretch.
// Plain sleep_for(1ms) on Windows really sleeps ~15.6 ms -- a whole game tick,
// which is enough on its own to make bhop miss every landing.
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
bool key_down(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

// Which button slots the user is physically holding, as a bitmask over kRel.
int held_mask() {
    int m = 0;
    if (key_down(VK_SHIFT))   m |= 1 << 0;
    if (key_down('R'))        m |= 1 << 1;
    if (key_down(VK_LBUTTON)) m |= 1 << 2;
    if (key_down(VK_RBUTTON)) m |= 1 << 3;
    if (key_down('W'))        m |= 1 << 6;
    if (key_down('S'))        m |= 1 << 7;
    if (key_down('A'))        m |= 1 << 8;
    if (key_down('D'))        m |= 1 << 9;
    if (key_down('E'))        m |= 1 << 10;
    if (key_down(VK_SPACE))   m |= 1 << 11;
    if (key_down(VK_CONTROL)) m |= 1 << 12;
    return m;
}

// Every button state is 0 when untouched, 256 after a release, 65537 while
// pressed. Anything else means this is not the button block.
bool plausible_dw(uint32_t v) {
    switch (v) {
        case 0x0u: case 0x1u: case 0x100u:
        case 0x101u: case 0x10000u: case 0x10001u: return true;
        default: return false;
    }
}

// Validates the whole 13-slot block anchored at j.
//   * a slot the user IS holding must have its down bit set
//   * a slot the user is NOT holding must read exactly 0 or 256
//   * require_evidence additionally demands two non-zero slots, so a long run
//     of zeros cannot masquerade as a button block
bool block_valid(uintptr_t j, int mask, bool require_evidence) {
    int pressed = 0, released_nonzero = 0;
    for (int i = 0; i < kSlotCount; ++i) {
        const uint32_t v = g_mem.read<uint32_t>(j + kRel[i]);
        if (!plausible_dw(v)) return false;
        if ((mask >> i) & 1) {
            if ((v & 1u) == 0u) return false;
            ++pressed;
        } else {
            if (v & 1u) return false;
            if (v) ++released_nonzero;
        }
    }
    if (require_evidence && pressed == 0 && released_nonzero < 2) return false;
    return true;
}

// Decisive path: find a dword with its down bit set, then work out where the
// block's jump slot would have to be for the held key to explain it. This uses
// YOUR key presses as proof, so it cannot lock onto a coincidence.
uintptr_t scan_pressed(int mask) {
    const uintptr_t lo = g_mem.client_dll + offsets::kJumpScanLo;
    const uintptr_t hi = g_mem.client_dll + offsets::kJumpScanHi;
    constexpr size_t PIECE = 0x40000;

    static std::vector<uint8_t> buf;
    if (buf.size() < PIECE) buf.resize(PIECE);

    for (uintptr_t a = lo; a + PIECE <= hi; a += PIECE) {
        if (g_stop.load()) return 0;
        if (held_mask() != mask) return 0;      // keys changed, retry later
        if (!g_mem.read_bytes(a, buf.data(), PIECE)) continue;

        for (size_t k = 0; k + 4 <= PIECE; k += 4) {
            uint32_t v = 0;
            std::memcpy(&v, buf.data() + k, 4);
            if ((v & 1u) == 0u) continue;
            if (!plausible_dw(v)) continue;

            const uintptr_t p = a + k;

            // If this dword is slot i of a block, the jump address is p-kRel[i].
            for (int i = 0; i < kSlotCount; ++i) {
                if (!((mask >> i) & 1)) continue;
                const uintptr_t jump =
                    p - static_cast<uintptr_t>(kRel[i]);
                if (jump < g_mem.client_dll) continue;
                if (!block_valid(jump, mask, false)) continue;
                return jump;
            }
        }
    }
    return 0;
}

// Nothing held: collect candidates with a bitmap over the already-read page so
// the sweep costs almost no syscalls. These are only candidates -- they still
// need a held key to confirm.
void collect_candidates(std::vector<uintptr_t>& out) {
    out.clear();
    const uintptr_t lo = g_mem.client_dll + offsets::kJumpScanLo;
    const uintptr_t hi = g_mem.client_dll + offsets::kJumpScanHi;
    constexpr size_t PIECE = 0x40000;

    static std::vector<uint8_t> buf, flag;
    if (buf.size() < PIECE) { buf.resize(PIECE); flag.resize(PIECE / 4); }

    for (uintptr_t a = lo; a + PIECE <= hi; a += PIECE) {
        if (g_stop.load()) return;
        if (!g_mem.read_bytes(a, buf.data(), PIECE)) continue;

        for (size_t k = 0; k + 4 <= PIECE; k += 4) {
            uint32_t v = 0;
            std::memcpy(&v, buf.data() + k, 4);
            flag[k / 4] = plausible_dw(v) ? 1 : 0;
        }

        for (size_t off = 0x640; off + 0x94 <= PIECE; off += 4) {
            bool ok = true;
            for (int i = 0; i < kSlotCount && ok; ++i) {
                const intptr_t pos = static_cast<intptr_t>(off) + kRel[i];
                if (pos < 0 || pos + 4 > static_cast<intptr_t>(PIECE)) ok = false;
                else if (!flag[pos / 4]) ok = false;
            }
            if (!ok) continue;
            out.push_back(a + off);
            if (out.size() >= 32) return;
        }
    }
}

void scan_thread_main() {
    for (int i = 0; i < 150 && !g_stop.load(); ++i) wait_ms(10);

    std::vector<uintptr_t> candidates;

    while (!g_stop.load()) {
        if (g_locked.load()) { wait_ms(500); continue; }

        g_scanning.store(true);

        const int mask = held_mask();
        uintptr_t hit = 0;

        // 1. known hints, confirmed against held keys
        for (uintptr_t off : kCandidates) {
            const uintptr_t abs = g_mem.client_dll + off;
            if (block_valid(abs, mask, mask == 0)) { hit = abs; break; }
        }

        // 2. decisive: a key you are holding proves the block
        if (!hit && mask) hit = scan_pressed(mask);

        // 3. candidates gathered while idle, now confirmable
        if (!hit && mask) {
            for (uintptr_t c : candidates)
                if (block_valid(c, mask, false)) { hit = c; break; }
        }

        // 4. refresh candidates (cheap, no per-candidate syscalls yet)
        if (!hit && !mask) {
            collect_candidates(candidates);
            g_dbg_cands.store(static_cast<int>(candidates.size()));
        }

        if (hit) {
            g_jump_offset.store(hit - g_mem.client_dll);
            g_locked.store(true);
        }
        g_scanning.store(false);

        if (!g_locked.load()) wait_ms(mask ? 50 : 300);
    }
}

// ── ground state, graded against the local player's Z ────────────────────

struct GroundWatch {
    bool   have_z = false;
    float  z = 0.0f;
    double z_changed = 0.0;
    bool   z_ground = true;
    int    flag_g = 0, flag_a = 0;
    int    hge_g = 0, hge_a = 0;
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

    // A signal is trusted only after it has been seen in BOTH states.
    if (!g_gw.flag_ok && g_gw.flag_g >= 6 && g_gw.flag_a >= 6) g_gw.flag_ok = true;
    if (!g_gw.hge_ok  && g_gw.hge_g  >= 6 && g_gw.hge_a  >= 6) g_gw.hge_ok  = true;

    int sig = 1;
    if (g_gw.flag_ok) sig |= 2;
    if (g_gw.hge_ok)  sig |= 4;
    g_dbg_signals.store(sig);

    // The flag is instance-accurate where the Z test needs a 22 ms window, so
    // once it has proven itself it becomes the primary signal. That matters for
    // bhop, where the grounded frame can be a single tick.
    bool ground;
    if (g_gw.flag_ok) ground = (fl & 1u) != 0u;
    else              ground = g_gw.z_ground;

    if (g_gw.hge_ok) ground = ground || (hge != 0xFFFFFFFFu);

    g_dbg_ground.store(ground);
    return ground;
}

// ── input mode: no offsets, cannot go stale ──────────────────────────────
// Hold the synthetic spacebar while grounded, release while airborne. That is
// the macro a bhop script performs. Deliberately does NOT read the spacebar,
// because SendInput feeds back into GetAsyncKeyState and would latch itself on.
void inject_space(bool down) {
    static bool s_down = false;
    if (down == s_down) return;      // only send on change
    s_down = down;

    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = VK_SPACE;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_SPACE, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = down ? 0u : KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(INPUT));
}

void bhop_thread_main() {
    if (g_mem.is_valid())
        g_mem.write<int32_t>(g_mem.client_dll + g_jump_offset.load(), kJumpRelease);

    uintptr_t pawn = 0;
    double    pawn_at = 0.0, last_sample = 0.0;
    bool      ground = false;

    while (!g_stop.load()) {
        wait_ms(1);
        if (!g_mem.is_valid()) continue;

        const bool focused = cs2_focused();
        g_dbg_focused.store(focused);

        // Outside the game, make sure nothing is left held down.
        if (!focused) {
            if (g_input_mode.load()) inject_space(false);
            else g_mem.write<int32_t>(g_mem.client_dll + g_jump_offset.load(),
                                      kJumpRelease);
            continue;
        }

        const double now = now_ms();
        if (!pawn || now - pawn_at > 500.0) {
            pawn = g_mem.read<uintptr_t>(
                g_mem.client_dll + offsets::dwLocalPlayerPawn);
            pawn_at = now;
        }
        if (!pawn) continue;

        // 2 ms sampling: the grounded frame can be a single 15.6 ms tick, so
        // this needs to be comfortably faster than a tick.
        if (now - last_sample >= 2.0) {
            last_sample = now;
            ground = sample_ground(g_mem, pawn);
        }

        if (g_input_mode.load()) {
            // Grounded -> hold jump, which re-jumps on every landing.
            // Airborne -> release, so the next landing is a fresh press edge.
            inject_space(ground);
        } else {
            if (!g_locked.load()) continue;   // never write to an unconfirmed address
            const uintptr_t jump_addr =
                g_mem.client_dll + g_jump_offset.load();
            g_mem.write<int32_t>(jump_addr, ground ? kJumpPress : kJumpRelease);
        }
    }

    if (g_input_mode.load()) inject_space(false);
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
    inject_space(false);
    if (g_bhop_thread.joinable()) g_bhop_thread.join();
    if (g_scan_thread.joinable()) g_scan_thread.join();
    g_started.store(false);
}

BhopDebug Bhop_GetDebug() {
    BhopDebug d;
    d.offset     = g_jump_offset.load();
    d.on_ground  = g_dbg_ground.load();
    d.focused    = g_dbg_focused.load();
    d.signals    = g_dbg_signals.load();
    d.locked     = g_locked.load();
    d.scanning   = g_scanning.load();
    d.candidates = g_dbg_cands.load();
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
