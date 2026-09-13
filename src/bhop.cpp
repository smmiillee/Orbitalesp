// --- src/bhop.cpp ---
// Bhop on HOLD-SPACE. The held key is a GATE; the jump itself is commanded by
// us, either through the jump button in memory (precise) or through synthetic
// keystrokes (no writes, but jittery).
//
//   +jump = 65537 (0x10001)   -jump = 256 (0x00000100)
//
// The button block is 13 dwords on a 0x90 stride (memory mode only):
//   sprint -0x630  reload -0x5A0  attack -0x510  attack2 -0x480
//   turnleft -0x3F0 turnright -0x360 forward -0x2D0 back -0x240
//   left -0x1B0     right -0x120     use -0x090      JUMP   duck +0x090
//
// ── THE THREE THINGS THAT MAKE THIS NOT MISS ─────────────────────────────
// 1. Deterministic output. A memory write is microseconds; SendInput is not.
// 2. The physical spacebar is swallowed while we drive, so the engine's own
//    input poll cannot re-assert your held key and erase the press edge.
// 3. A guaranteed press window: once grounded we hold the press for ~2 ticks,
//    which survives the race where the engine's own button write lands just
//    after ours.
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

// How long a press is held once we are grounded. Two ticks at 64 Hz. This is
// what covers the race against the engine rewriting the button from input.
constexpr double kPressHoldMs = 30.0;

// Memory-mode hints only. A lock still has to be confirmed by held input.
constexpr uintptr_t kCandidates[] = {
    0x2095490, 0x2096490, 0x2094490, 0x2093490, 0x205BAF0,
};

std::atomic<bool>      g_stop{false}, g_started{false};
std::atomic<bool>      g_locked{false}, g_scanning{false};
std::atomic<int>       g_mode{BHOP_MEMORY};
std::atomic<uintptr_t> g_jump_offset{offsets::dwForceJump};

// Diagnostics.
std::atomic<bool> g_dbg_ground{false},  g_dbg_focused{false};
std::atomic<bool> g_dbg_space{false},   g_dbg_driving{false};
std::atomic<bool> g_dbg_suppress{false};
std::atomic<int>  g_dbg_signals{0},     g_dbg_presses{0};

// Keyboard hook.
std::atomic<bool>  g_phys_space{false};   // physical (non-injected) spacebar
std::atomic<bool>  g_hook_ok{false};
std::atomic<bool>  g_suppress{false};     // swallow the physical spacebar
std::atomic<DWORD> g_hook_tid{0};
HHOOK g_hook = nullptr;

std::thread g_bhop_thread, g_scan_thread, g_hook_thread;

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

// ── keyboard hook ────────────────────────────────────────────────────────
// Two jobs:
//   1. track the PHYSICAL spacebar. Injected events carry LLKHF_INJECTED, so
//      ours are ignored -- this is the only way to read your real key while we
//      are injecting.
//   2. while bhop is driving, SWALLOW the physical spacebar so the game's jump
//      input comes only from us. Without this, auto-repeat re-asserts your DOWN
//      against our release and the landing press edge never forms.
//
// Scoped to "bhop is driving", so space behaves normally everywhere else.
LRESULT CALLBACK kb_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION) {
        const auto* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
        if (k && k->vkCode == VK_SPACE && (k->flags & LLKHF_INJECTED) == 0) {
            switch (wparam) {
                case WM_KEYDOWN:
                case WM_SYSKEYDOWN:
                    g_phys_space.store(true);
                    break;
                case WM_KEYUP:
                case WM_SYSKEYUP:
                    g_phys_space.store(false);
                    break;
                default:
                    break;
            }
            if (g_suppress.load())
                return 1;   // swallow: the game must only see our own input
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

void hook_thread_main() {
    g_hook_tid.store(GetCurrentThreadId());

    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, kb_proc, nullptr, 0);
    g_hook_ok.store(g_hook != nullptr);

    // A low-level hook is delivered on the thread that installed it, so this
    // thread must pump messages or the callback never runs.
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
    g_hook_ok.store(false);
}

// Physical space state. The hook is authoritative; GetAsyncKeyState is only a
// fallback if the hook could not be installed.
bool space_held() {
    if (g_hook_ok.load()) return g_phys_space.load();
    return (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
}

// ── held-input mask (memory-mode address proof) ──────────────────────────
// Space MUST be included: while you hold it the jump slot legitimately reads
// 0x10001. Excluding it made every candidate fail validation, which is why
// memory mode could never lock.
int held_mask() {
    auto down = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    int m = 0;
    if (down(VK_SHIFT))   m |= 1 << 0;
    if (down('R'))        m |= 1 << 1;
    if (down(VK_LBUTTON)) m |= 1 << 2;
    if (down(VK_RBUTTON)) m |= 1 << 3;
    if (down('W'))        m |= 1 << 6;
    if (down('S'))        m |= 1 << 7;
    if (down('A'))        m |= 1 << 8;
    if (down('D'))        m |= 1 << 9;
    if (down('E'))        m |= 1 << 10;
    if (space_held())     m |= 1 << kJumpSlot;
    return m;
}

// ── memory-mode jump-button scanner ──────────────────────────────────────

bool plausible_dw(uint32_t v) {
    switch (v) {
        case 0x0u: case 0x1u: case 0x100u:
        case 0x101u: case 0x10000u: case 0x10001u: return true;
        default: return false;
    }
}

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
// block's jump slot would have to be to explain it. Uses YOUR held keys as
// proof, so it cannot lock onto a coincidence.
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
            for (int i = 0; i < kSlotCount; ++i) {
                if (!((mask >> i) & 1)) continue;
                const uintptr_t jump = p - static_cast<uintptr_t>(kRel[i]);
                if (jump < g_mem.client_dll) continue;
                if (!block_valid(jump, mask, false)) continue;
                return jump;
            }
        }
    }
    return 0;
}

void scan_thread_main() {
    for (int i = 0; i < 150 && !g_stop.load(); ++i) wait_ms(10);

    while (!g_stop.load()) {
        if (g_locked.load()) { wait_ms(500); continue; }

        g_scanning.store(true);

        const int mask = held_mask();
        uintptr_t hit = 0;

        for (uintptr_t off : kCandidates) {
            const uintptr_t abs = g_mem.client_dll + off;
            if (block_valid(abs, mask, false)) { hit = abs; break; }
        }
        if (!hit && mask) hit = scan_pressed(mask);

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

    // A signal is trusted only after it has been seen in BOTH states, so a
    // wrong offset degrades the verdict instead of breaking it.
    if (!g_gw.flag_ok && g_gw.flag_g >= 6 && g_gw.flag_a >= 6) g_gw.flag_ok = true;
    if (!g_gw.hge_ok  && g_gw.hge_g  >= 6 && g_gw.hge_a  >= 6) g_gw.hge_ok  = true;

    int sig = 1;
    if (g_gw.flag_ok) sig |= 2;
    if (g_gw.hge_ok)  sig |= 4;
    g_dbg_signals.store(sig);

    // The flag is instance-accurate where the Z test needs a 22 ms window, so
    // once proven it becomes primary. That matters here: the grounded frame can
    // be a single tick.
    bool ground;
    if (g_gw.flag_ok) ground = (fl & 1u) != 0u;
    else              ground = g_gw.z_ground;
    if (g_gw.hge_ok) ground = ground || (hge != 0xFFFFFFFFu);

    g_dbg_ground.store(ground);
    return ground;
}

// ── output: one place where the two modes diverge ────────────────────────

// Send only on a state CHANGE, so injection produces clean edges rather than a
// stream of redundant keystrokes.
void inject_space(bool down) {
    static bool s_down = false;
    if (down == s_down) return;
    s_down = down;

    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = VK_SPACE;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_SPACE, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = down ? 0u : KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(INPUT));
}

void write_button(bool down) {
    if (!g_locked.load() || !g_mem.is_valid()) return;
    g_mem.write<int32_t>(g_mem.client_dll + g_jump_offset.load(),
                         down ? kJumpPress : kJumpRelease);
}

void bhop_thread_main() {
    // Memory mode only: park the button released so a stale value can't wedge
    // the jump. Injection mode performs no writes at all.
    if (g_mode.load() == BHOP_MEMORY && g_mem.is_valid())
        g_mem.write<int32_t>(g_mem.client_dll + g_jump_offset.load(),
                             kJumpRelease);

    uintptr_t pawn = 0;
    double    pawn_at = 0.0, last_sample = 0.0;
    bool      ground = false, prev_ground = false;
    bool      out_down = false;
    double    press_until = 0.0;

    while (!g_stop.load()) {
        wait_ms(1);
        if (!g_mem.is_valid()) continue;

        const int  mode    = g_mode.load();
        const bool focused = cs2_focused();
        const bool space   = space_held();

        g_dbg_focused.store(focused);
        g_dbg_space.store(space);

        const bool active = (mode != BHOP_OFF) && focused && space;

        // In memory mode we can only act once the address is confirmed, so we
        // must not swallow the key before then -- otherwise your normal jump
        // would stop working while we waited.
        const bool can_drive =
            (mode == BHOP_INJECT) ||
            (mode == BHOP_MEMORY && g_locked.load());

        const bool suppress = active && can_drive;
        g_suppress.store(suppress);
        g_dbg_suppress.store(suppress);

        // ── idle: always leave the world in a released state ─────────────
        if (!active || !can_drive) {
            if (out_down && mode == BHOP_INJECT) inject_space(false);
            if (mode == BHOP_MEMORY) write_button(false);
            out_down = false;
            press_until = 0.0;
            prev_ground = false;
            g_dbg_driving.store(false);
            continue;
        }

        const double now = now_ms();
        if (!pawn || now - pawn_at > 500.0) {
            pawn = g_mem.read<uintptr_t>(
                g_mem.client_dll + offsets::dwLocalPlayerPawn);
            pawn_at = now;
        }
        if (!pawn) {
            if (out_down && mode == BHOP_INJECT) inject_space(false);
            if (mode == BHOP_MEMORY) write_button(false);
            out_down = false;
            continue;
        }

        // 2 ms sampling: the grounded frame can be a single 15.6 ms tick, so
        // this has to be comfortably faster than a tick.
        if (now - last_sample >= 2.0) {
            last_sample = now;
            ground = sample_ground(g_mem, pawn);
        }

        // ── THE JUMP PATTERN ─────────────────────────────────────────────
        // On the rising edge of "grounded", open a guaranteed press window.
        // While airborne the button stays released, which is what makes the
        // next press a fresh edge. Both are height- and surface-independent:
        // a landing off a roof reads exactly like a landing on flat ground.
        if (ground && !prev_ground) {
            press_until = now + kPressHoldMs;
            g_dbg_presses.fetch_add(1);
        }
        prev_ground = ground;

        const bool want = ground || (now < press_until);

        if (mode == BHOP_MEMORY) {
            // Re-assert EVERY iteration: the engine rewrites the button from
            // input each tick, so a single write would be overwritten. This is
            // also what makes the press window meaningful.
            write_button(want);
        } else if (want != out_down) {
            inject_space(want);
        }
        out_down = want;
        g_dbg_driving.store(true);
    }

    // Never strand a held key or a stuck button on shutdown.
    g_suppress.store(false);
    if (g_mode.load() == BHOP_INJECT) inject_space(false);
    if (g_mode.load() == BHOP_MEMORY) write_button(false);
}

} // namespace

void Bhop_Init() {
    if (!g_mem.is_valid()) return;
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;

    g_stop.store(false);

    // The hook is required for hold-space to behave, so start it first.
    g_hook_thread = std::thread(hook_thread_main);
    g_bhop_thread = std::thread(bhop_thread_main);
    g_scan_thread = std::thread(scan_thread_main);
}

void Bhop_Shutdown() {
    g_stop.store(true);
    g_suppress.store(false);
    inject_space(false);
    if (g_mode.load() == BHOP_MEMORY) write_button(false);

    if (g_hook_tid.load())
        PostThreadMessageW(g_hook_tid.load(), WM_QUIT, 0, 0);

    if (g_bhop_thread.joinable()) g_bhop_thread.join();
    if (g_scan_thread.joinable()) g_scan_thread.join();
    if (g_hook_thread.joinable()) g_hook_thread.join();
    g_started.store(false);
}

BhopDebug Bhop_GetDebug() {
    BhopDebug d;
    d.mode        = g_mode.load();
    d.offset      = g_jump_offset.load();
    d.on_ground   = g_dbg_ground.load();
    d.focused     = g_dbg_focused.load();
    d.space_held  = g_dbg_space.load();
    d.hook_ok     = g_hook_ok.load();
    d.driving     = g_dbg_driving.load();
    d.suppressing = g_dbg_suppress.load();
    d.signals     = g_dbg_signals.load();
    d.presses     = g_dbg_presses.load();
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

int  Bhop_Mode() { return g_mode.load(); }

void Bhop_SetMode(int mode) {
    g_mode.store(mode);
    // Leaving a driving mode must not strand the game with a held key.
    if (mode != BHOP_INJECT) inject_space(false);
    g_suppress.store(false);
}
