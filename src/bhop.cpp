// --- src/bhop.cpp ---
// One bhop engine. SPACE only. No memory writes.
//
// ══ THE BUG THIS FIXES ══════════════════════════════════════════════════
// The old code did this:
//
//     INPUT in[2]{};  in[0] = down;  in[1] = up;
//     SendInput(2, in, sizeof(INPUT));          // down+up, zero duration
//
// Both transitions land in the same call, so the key is down for ~0 us. CS2
// samples keyboard state once per frame, so such a press is frequently never
// seen by any frame sample. The input is not late -- it is invisible.
//
// ══ THE MODEL ═══════════════════════════════════════════════════════════
//   * The server ticks (64/128). Subtick adds sub-tick timestamps on top.
//   * The CLIENT computes those timestamps from its own per-frame input read.
//   * Therefore the clock that limits us is FRAME time, not tick time.
//
// So a press must be HELD long enough to be sampled by at least one frame:
// down -> hold_ms -> up. Then retry, because being grounded means the previous
// hop failed and we need another attempt.
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

std::atomic<bool>   g_stop{false}, g_started{false};
std::atomic<bool>   g_enabled{false};
std::atomic<double> g_hold_ms{10.0};    // spans a frame at up to ~100 fps
std::atomic<double> g_retry_ms{16.0};   // about one 64-tick interval

// Diagnostics.
std::atomic<bool>   g_dbg_focus{false}, g_dbg_space{false};
std::atomic<bool>   g_dbg_hook{false},  g_dbg_ground{false};
std::atomic<bool>   g_dbg_suppress{false}, g_dbg_pressing{false};
std::atomic<int>    g_dbg_signals{0},   g_dbg_inj{0};
std::atomic<double> g_dbg_last{0.0},    g_dbg_hold_used{0.0};

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
// are ignored) and swallows it while the engine drives. A held +jump cannot
// produce a new press edge, so our injected edges must be the only ones the
// game sees.
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

    // Bitmask on the flag, never a whole-value comparison.
    bool ground;
    if (g_gw.flag_ok) ground = (fl & 1u) != 0u;
    else              ground = g_gw.z_ground;
    if (g_gw.hge_ok) ground = ground || (hge != 0xFFFFFFFFu);

    g_dbg_ground.store(ground);
    return ground;
}

// ── injection: SEPARATE down and up transitions ──────────────────────────
// This is the whole fix. The key goes down, stays down for hold_ms so at least
// one frame samples it, and only then goes up.
void key_down() {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = VK_SPACE;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_SPACE, MAPVK_VK_TO_VSC));
    in.ki.dwExtraInfo = 0;
    in.ki.time = 0;
    SendInput(1, &in, sizeof(INPUT));
}

void key_up() {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = VK_SPACE;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_SPACE, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = KEYEVENTF_KEYUP;
    in.ki.dwExtraInfo = 0;
    in.ki.time = 0;
    SendInput(1, &in, sizeof(INPUT));
}

void run_thread() {
    uintptr_t pawn = 0;
    double    pawn_at = 0.0, last_sample = 0.0;
    bool      ground = false, prev_ground = false;

    bool   pressed   = false;
    double press_at  = 0.0;
    double next_press = 0.0;

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

        // ── not driving: release cleanly and reset ───────────────────────
        if (!driving) {
            if (pressed) { key_up(); pressed = false; }
            next_press = 0.0;
            prev_ground = false;
            g_dbg_pressing.store(false);
            continue;
        }

        const double now = now_ms();
        if (!pawn || now - pawn_at > 500.0) {
            pawn = g_mem.read<uintptr_t>(
                g_mem.client_dll + offsets::dwLocalPlayerPawn);
            pawn_at = now;
        }
        if (!pawn) {
            if (pressed) { key_up(); pressed = false; }
            continue;
        }

        if (now - last_sample >= 2.0) {
            last_sample = now;
            ground = sample_ground(g_mem, pawn);
        }

        const double hold_ms  = g_hold_ms.load();
        const double retry_ms = g_retry_ms.load();

        // ── release after the hold has elapsed ───────────────────────────
        // Released as soon as the hold is up, regardless of ground state, so no
        // matter what happens next the next press is a fresh edge.
        if (pressed && (now - press_at) >= hold_ms) {
            key_up();
            pressed = false;
            g_dbg_hold_used.store(now - press_at);
        }

        if (!ground) {
            // Airborne: make sure we are not holding into the landing, because
            // the landing press must be a clean rising edge.
            if (pressed) { key_up(); pressed = false; }
            next_press = 0.0;
            prev_ground = false;
            g_dbg_pressing.store(false);
            continue;
        }

        // ── grounded: press, and keep retrying ───────────────────────────
        // Retrying is what stops a missed hop from ending the chain: if we are
        // still grounded, the previous attempt did not take, so try again.
        if (!pressed) {
            if (next_press <= 0.0) next_press = now;
            if (now >= next_press) {
                key_down();
                pressed  = true;
                press_at = now;
                next_press = now + retry_ms;
                g_dbg_inj.fetch_add(1);
                g_dbg_last.store(now);
            }
        }

        prev_ground = ground;
        g_dbg_pressing.store(pressed);
    }

    if (pressed) key_up();
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
    d.pressing    = g_dbg_pressing.load();
    d.signals     = g_dbg_signals.load();
    d.injected    = g_dbg_inj.load();

    const double last = g_dbg_last.load();
    d.age_ms = (last <= 0.0) ? -1 : static_cast<int>(now_ms() - last);

    d.hold_ms  = static_cast<float>(g_dbg_hold_used.load());
    d.retry_ms = static_cast<float>(g_retry_ms.load());
    return d;
}

void  Bhop_SetEnabled(bool on) { g_enabled.store(on); }
bool  Bhop_Enabled()           { return g_enabled.load(); }

void  Bhop_SetHoldMs(float ms) {
    if (ms < 1.0f)  ms = 1.0f;
    if (ms > 40.0f) ms = 40.0f;
    g_hold_ms.store(static_cast<double>(ms));
}
float Bhop_HoldMs() { return static_cast<float>(g_hold_ms.load()); }

void  Bhop_SetRetryMs(float ms) {
    if (ms < 2.0f)  ms = 2.0f;
    if (ms > 60.0f) ms = 60.0f;
    g_retry_ms.store(static_cast<double>(ms));
}
float Bhop_RetryMs() { return static_cast<float>(g_retry_ms.load()); }
