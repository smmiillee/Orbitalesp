// --- src/bhop.cpp ---
// One bhop engine. SPACE only. No memory writes.
//
// ══ WHY THE PREVIOUS TIMING DRIFTED ═════════════════════════════════════
// The engine injected a pair, then scheduled the next at "now + 15.625 ms".
// But `now` is when WE noticed the flag change, and detection lands somewhere
// inside a tick -- up to one 2 ms sample late, and the flag itself is only
// written once per 15.625 ms tick. So each retry carried a random phase offset,
// and successive presses wandered around the tick instead of landing on it.
//
// ══ THE FIX ═════════════════════════════════════════════════════════════
// The ground flag can only change ON a tick boundary. So we timestamp every
// transition, infer the tick period and phase from them, and then schedule each
// press to land AT a predicted boundary rather than at a fixed interval.
//
//   observe(t)      -> refine period and the position of a known boundary
//   next_after(t)   -> the first boundary strictly after t
//
// The offset slider then shifts where inside the tick the pair lands, which is
// the one thing that actually needs dialling in per machine.
#include "bhop.h"
#include "memory.h"
#include "offsets.h"

#include <Windows.h>
#include <mmsystem.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>

#pragma comment(lib, "winmm.lib")

extern HWND g_cs2_hwnd;

namespace {

// 64-tick default, refined at runtime.
constexpr double kDefaultTickMs = 15.625;

std::atomic<bool>   g_stop{false}, g_started{false};
std::atomic<bool>   g_enabled{false};
std::atomic<bool>   g_tick_lock{true};
std::atomic<double> g_offset_ms{0.0};
std::atomic<int>    g_retry_ticks{1};

// Diagnostics.
std::atomic<bool>  g_dbg_focus{false}, g_dbg_space{false};
std::atomic<bool>  g_dbg_hook{false},  g_dbg_ground{false};
std::atomic<bool>  g_dbg_suppress{false}, g_dbg_locked{false};
std::atomic<int>   g_dbg_signals{0},   g_dbg_inj{0};
std::atomic<double> g_dbg_last{0.0},   g_dbg_tick{kDefaultTickMs};
std::atomic<double> g_dbg_phase{-1.0};

// Keyboard hook.
std::atomic<bool>  g_phys_space{false};
std::atomic<bool>  g_suppress{false};
std::atomic<DWORD> g_hook_tid{0};
HHOOK g_hook = nullptr;

std::thread g_run_thread, g_hook_thread;

double now_ms() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0).count();
}

void wait_ms(double ms) {
    if (ms <= 0.0) return;
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::microseconds(static_cast<long long>(ms * 1000.0));
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
// Tracks the PHYSICAL spacebar (injected events carry LLKHF_INJECTED, so ours
// are ignored) and swallows it while the engine drives, because a held +jump
// cannot produce a new press edge.
LRESULT CALLBACK kb_proc(int code, WPARAM wparam, LPARAM lparam) {
    if (code == HC_ACTION) {
        const auto* k = reinterpret_cast<KBDLLHOOKSTRUCT*>(lparam);
        if (k && k->vkCode == VK_SPACE && (k->flags & LLKHF_INJECTED) == 0) {
            switch (wparam) {
                case WM_KEYDOWN: case WM_SYSKEYDOWN:
                    g_phys_space.store(true);  break;
                case WM_KEYUP:   case WM_SYSKEYUP:
                    g_phys_space.store(false); break;
                default: break;
            }
            if (g_suppress.load()) return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

void hook_thread_main() {
    g_hook_tid.store(GetCurrentThreadId());
    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, kb_proc, nullptr, 0);
    g_dbg_hook.store(g_hook != nullptr);

    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
    g_dbg_hook.store(false);
}

bool space_held() {
    if (g_dbg_hook.load()) return g_phys_space.load();
    return (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
}

// ── ground state ─────────────────────────────────────────────────────────
struct GroundWatch {
    bool   have_z = false;
    float  z = 0.0f;
    double z_changed = 0.0;
    bool   z_ground = true;
    int    flag_g = 0, flag_a = 0, hge_g = 0, hge_a = 0;
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

    if (!g_gw.flag_ok && g_gw.flag_g >= 6 && g_gw.flag_a >= 6) g_gw.flag_ok = true;
    if (!g_gw.hge_ok  && g_gw.hge_g  >= 6 && g_gw.hge_a  >= 6) g_gw.hge_ok  = true;

    int sig = 1;
    if (g_gw.flag_ok) sig |= 2;
    if (g_gw.hge_ok)  sig |= 4;
    g_dbg_signals.store(sig);

    bool ground;
    if (g_gw.flag_ok) ground = (fl & 1u) != 0u;
    else              ground = g_gw.z_ground;
    if (g_gw.hge_ok) ground = ground || (hge != 0xFFFFFFFFu);

    g_dbg_ground.store(ground);
    return ground;
}

// ── tick clock ───────────────────────────────────────────────────────────
// Ground transitions can only occur on tick boundaries, so we timestamp them,
// infer the period and phase, and predict future boundaries. This is what
// replaces "press 15.625 ms after we noticed".
struct TickClock {
    bool   have       = false;
    bool   converged  = false;
    double period     = kDefaultTickMs;
    double boundary   = 0.0;    // a boundary we believe in
    double observed   = 0.0;    // last transition we fed in
    int    samples    = 0;

    // Feed a transition timestamp.
    void observe(double t) {
        if (!have) {
            have = true;
            boundary = t;
            observed = t;
            return;
        }

        const double dt = t - observed;
        observed = t;

        // Ignore gaps that aren't a plausible whole number of ticks.
        if (dt < period * 0.4 || dt > period * 40.0) return;

        const double n = std::floor(dt / period + 0.5);
        if (n < 1.0) return;

        // Refine the period toward the measured per-tick length.
        const double measured = dt / n;
        period = period * 0.8 + measured * 0.2;
        if (period < 4.0)   period = 4.0;
        if (period > 40.0)  period = 40.0;

        // The transition happened at the boundary just before `t`, minus our
        // detection latency. Re-anchor the grid to that boundary.
        boundary = t;
        if (++samples >= 4) converged = true;

        g_dbg_tick.store(period);
        g_dbg_locked.store(converged);
    }

    // First boundary strictly after t. If we have no grid, just return t.
    double next_after(double t, double offset) const {
        if (!have) return t + offset;

        double b = boundary;
        // Walk forward in tick steps until we pass t.
        if (b <= t) {
            const double need = (t - b) / period;
            b += period * (std::floor(need) + 1.0);
        }
        return b + offset;
    }

    // Phase of t within the estimated tick, in ms. For diagnostics.
    double phase_of(double t) const {
        if (!have) return -1.0;
        double d = std::fmod(t - boundary, period);
        if (d < 0.0) d += period;
        return d;
    }
};
TickClock g_clock;

// ── injection ────────────────────────────────────────────────────────────
// SPACE only. Because the physical key is swallowed while driving, this is the
// only space input the game sees -- so each pair is a clean, unambiguous jump.
void key_pair() {
    INPUT in[2]{};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = VK_SPACE;
    in[0].ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_SPACE, MAPVK_VK_TO_VSC));
    in[0].ki.dwExtraInfo = 0;
    in[0].ki.time = 0;

    in[1] = in[0];
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;

    SendInput(2, in, sizeof(INPUT));
}

void run_thread() {
    uintptr_t pawn = 0;
    double    pawn_at = 0.0, last_sample = 0.0;
    bool      ground = false, prev_ground = false;
    double    next_fire = 0.0;

    while (!g_stop.load()) {
        wait_ms(1.0);
        if (!g_mem.is_valid()) continue;

        const bool focused = cs2_focused();
        const bool space   = space_held();
        g_dbg_focus.store(focused);
        g_dbg_space.store(space);

        const bool driving = g_enabled.load() && focused && space;
        g_suppress.store(driving);
        g_dbg_suppress.store(driving);

        if (!driving) {
            next_fire = 0.0;
            prev_ground = false;
            continue;
        }

        const double now = now_ms();
        if (!pawn || now - pawn_at > 500.0) {
            pawn = g_mem.read<uintptr_t>(
                g_mem.client_dll + offsets::dwLocalPlayerPawn);
            pawn_at = now;
        }
        if (!pawn) continue;

        // 2 ms sampling: fine enough to place a transition within a tick.
        if (now - last_sample >= 2.0) {
            last_sample = now;
            const bool prev = ground;
            ground = sample_ground(g_mem, pawn);

            // Every ground transition is a tick boundary, so feed the clock.
            if (ground != prev) g_clock.observe(now);
        }

        const bool tick_lock = g_tick_lock.load();
        const double offset  = g_offset_ms.load();
        const int    retry   = g_retry_ticks.load();

        // ── LANDING: arm the first press ─────────────────────────────────
        // Rising edge of grounded. With tick lock we aim at the next boundary;
        // without it we fire immediately, which is the old behaviour.
        if (!prev_ground && ground) {
            next_fire = tick_lock ? g_clock.next_after(now, offset) : now;
        }
        prev_ground = ground;

        // ── PRESS DECISIONS ─────────────────────────────────────────────
        // While grounded, keep retrying: a missed hop leaves the flag true, and
        // retrying is what stops that from ending the chain. While airborne,
        // stay silent so the next landing is a fresh edge.
        if (ground) {
            if (next_fire <= 0.0) {
                next_fire = tick_lock ? g_clock.next_after(now, offset) : now;
            }
            if (now >= next_fire) {
                key_pair();
                g_dbg_inj.fetch_add(1);
                g_dbg_last.store(now);
                g_dbg_phase.store(g_clock.phase_of(now));

                // Schedule the retry on the tick grid, N ticks ahead.
                next_fire = tick_lock
                    ? g_clock.next_after(now + 0.1, offset) +
                          g_clock.period * (retry - 1)
                    : now + kDefaultTickMs * retry;
            }
        } else {
            // Airborne: cancel any pending press so the landing is clean.
            next_fire = 0.0;
        }
    }

    g_suppress.store(false);
}

} // namespace

void Bhop_Init() {
    if (!g_mem.is_valid()) return;
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;

    timeBeginPeriod(1);

    g_stop.store(false);
    g_hook_thread = std::thread(hook_thread_main);
    g_run_thread  = std::thread(run_thread);
}

void Bhop_Shutdown() {
    g_stop.store(true);
    g_suppress.store(false);

    if (g_hook_tid.load())
        PostThreadMessageW(g_hook_tid.load(), WM_QUIT, 0, 0);

    if (g_run_thread.joinable())  g_run_thread.join();
    if (g_hook_thread.joinable()) g_hook_thread.join();

    timeEndPeriod(1);
    g_started.store(false);
}

BhopDebug Bhop_GetDebug() {
    BhopDebug d;
    d.focused     = g_dbg_focus.load();
    d.space_held  = g_dbg_space.load();
    d.hook_ok     = g_dbg_hook.load();
    d.on_ground   = g_dbg_ground.load();
    d.suppressing = g_dbg_suppress.load();
    d.signals     = g_dbg_signals.load();
    d.injected    = g_dbg_inj.load();

    const double last = g_dbg_last.load();
    d.age_ms = (last <= 0.0) ? -1 : static_cast<int>(now_ms() - last);

    d.locked     = g_dbg_locked.load();
    d.tick_ms    = static_cast<float>(g_dbg_tick.load());
    d.last_phase = static_cast<float>(g_dbg_phase.load());
    return d;
}

void  Bhop_SetEnabled(bool on)   { g_enabled.store(on); }
bool  Bhop_Enabled()            { return g_enabled.load(); }

void  Bhop_SetTickLock(bool on)  { g_tick_lock.store(on); }
bool  Bhop_TickLock()           { return g_tick_lock.load(); }

void  Bhop_SetOffsetMs(float ms) {
    if (ms < 0.0f)  ms = 0.0f;
    if (ms > 20.0f) ms = 20.0f;
    g_offset_ms.store(static_cast<double>(ms));
}
float Bhop_OffsetMs() { return static_cast<float>(g_offset_ms.load()); }

void  Bhop_SetRetryTicks(int ticks) {
    if (ticks < 1) ticks = 1;
    if (ticks > 4) ticks = 4;
    g_retry_ticks.store(ticks);
}
int   Bhop_RetryTicks() { return g_retry_ticks.load(); }
