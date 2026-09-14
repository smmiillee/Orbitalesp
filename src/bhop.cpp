// --- src/bhop.cpp ---
// Bhop by INPUT INJECTION ONLY. Nothing in this file writes to cs2.exe.
//
//  SCROLL  - spam mouse-wheel events while the gate is held. Timing-agnostic:
//            with ~2 events per 15.6 ms tick, one almost certainly falls in the
//            landing window. This is the community-standard technique.
//  KEY     - press/release a key on each observed landing, as a second chance.
//
// The physical spacebar is swallowed while driving, because a held +jump cannot
// produce a new press edge and would block everything we inject.
#include "bhop.h"
#include "memory.h"
#include "offsets.h"

#include <Windows.h>
#include <mmsystem.h>      // timeBeginPeriod / timeEndPeriod
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <thread>

// So the project links without anyone having to touch CMakeLists.
#pragma comment(lib, "winmm.lib")

extern HWND g_cs2_hwnd;

namespace {

// One wheel event every 8 ms is ~125/s, i.e. ~2 per 64-tick, which is enough
// coverage without flooding the input queue.
constexpr double kDefaultScrollMs = 8.0;

// How long a KEY-mode press is held. Long enough to span the landing tick,
// short enough that the next landing still gets a fresh edge.
constexpr double kPressHoldMs = 30.0;

std::atomic<bool>   g_stop{false}, g_started{false};
std::atomic<bool>   g_enabled{true}, g_scroll{true}, g_key_inject{false};
std::atomic<double> g_interval_ms{kDefaultScrollMs};

// Diagnostics.
std::atomic<bool> g_dbg_ground{false},  g_dbg_focused{false};
std::atomic<bool> g_dbg_space{false},   g_dbg_driving{false};
std::atomic<bool> g_dbg_suppress{false}, g_dbg_hook{false};
std::atomic<int>  g_dbg_signals{0},     g_dbg_scrolls{0}, g_dbg_edges{0};

// Keyboard hook.
std::atomic<bool>  g_phys_space{false};   // physical (non-injected) spacebar
std::atomic<bool>  g_suppress{false};     // swallow the physical spacebar
std::atomic<DWORD> g_hook_tid{0};
HHOOK g_hook = nullptr;

std::thread g_run_thread, g_hook_thread;

double now_ms() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0).count();
}

// ~1 ms sleep that actually works. timeBeginPeriod(1) is what makes this honest;
// without it Windows quantises sleeps to ~15.6 ms, a whole game tick.
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
//   1. track the PHYSICAL spacebar. Injected events carry LLKHF_INJECTED, so
//      ours are ignored.
//   2. while driving, SWALLOW the physical spacebar so the engine's +jump
//      cannot stay held and block our injected edges.
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
                return 1;
        }
    }
    return CallNextHookEx(nullptr, code, wparam, lparam);
}

void hook_thread_main() {
    g_hook_tid.store(GetCurrentThreadId());

    g_hook = SetWindowsHookExW(WH_KEYBOARD_LL, kb_proc, nullptr, 0);
    g_dbg_hook.store(g_hook != nullptr);

    // A low-level hook is delivered on the thread that installed it, so this
    // must pump messages or the callback never runs.
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
    g_dbg_hook.store(false);
}

// Physical space state. The hook is authoritative; GetAsyncKeyState is only a
// fallback if the hook could not be installed.
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

    // A signal is trusted only after it has been seen in BOTH states.
    if (!g_gw.flag_ok && g_gw.flag_g >= 6 && g_gw.flag_a >= 6) g_gw.flag_ok = true;
    if (!g_gw.hge_ok  && g_gw.hge_g  >= 6 && g_gw.hge_a  >= 6) g_gw.hge_ok  = true;

    int sig = 1;
    if (g_gw.flag_ok) sig |= 2;
    if (g_gw.hge_ok)  sig |= 4;
    g_dbg_signals.store(sig);

    // The flag is instance-accurate where the Z test needs a 22 ms window.
    bool ground;
    if (g_gw.flag_ok) ground = (fl & 1u) != 0u;
    else              ground = g_gw.z_ground;
    if (g_gw.hge_ok) ground = ground || (hge != 0xFFFFFFFFu);

    g_dbg_ground.store(ground);
    return ground;
}

// ── injection ────────────────────────────────────────────────────────────

// One wheel notch. WHEEL_DELTA (120) is one click; negative scrolls down.
// With `bind mwheeldown +jh` in game this is a full press+release of +jump.
void inject_wheel() {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    in.mi.mouseData = static_cast<DWORD>(-WHEEL_DELTA);
    SendInput(1, &in, sizeof(INPUT));
}

// KEY mode. Send only on a state CHANGE so we produce clean edges.
void inject_key(bool down) {
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

void run_thread() {
    uintptr_t pawn = 0;
    double    pawn_at = 0.0, last_sample = 0.0, next_scroll = 0.0;
    bool      ground = false, out_down = false, prev_ground = false;
    double    press_until = 0.0;

    while (!g_stop.load()) {
        wait_ms(1);
        if (!g_mem.is_valid()) continue;

        const bool focused = cs2_focused();
        const bool space   = space_held();
        const bool active  = g_enabled.load() && focused && space;

        g_dbg_focused.store(focused);
        g_dbg_space.store(space);

        // Swallow the physical key only while we are actually driving, so space
        // behaves normally everywhere else.
        g_suppress.store(active);
        g_dbg_suppress.store(active);

        if (!active) {
            if (out_down) { inject_key(false); out_down = false; }
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
        if (!pawn) { if (out_down) { inject_key(false); out_down = false; } continue; }

        // Sample faster than a tick so a landing is never missed by more than
        // the tick itself.
        if (now - last_sample >= 2.0) {
            last_sample = now;
            ground = sample_ground(g_mem, pawn);
        }

        // ── SCROLL SPAM ──────────────────────────────────────────────────
        // Continuous while the gate is held. This is what gives coverage: we
        // are not trying to hit one tick, we are sweeping all of them.
        if (g_scroll.load() && now >= next_scroll) {
            next_scroll = now + g_interval_ms.load();
            inject_wheel();
            g_dbg_scrolls.fetch_add(1);
        }

        // ── KEY EDGE (optional second chance) ────────────────────────────
        if (g_key_inject.load()) {
            if (ground && !prev_ground) {
                press_until = now + kPressHoldMs;
                g_dbg_edges.fetch_add(1);
            }
            prev_ground = ground;

            const bool want = ground || (now < press_until);
            if (want != out_down) inject_key(want);
            out_down = want;
        }

        g_dbg_driving.store(true);
    }

    g_suppress.store(false);
    inject_key(false);
}

} // namespace

void Bhop_Init() {
    if (!g_mem.is_valid()) return;
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;

    // Ask Windows for a 1 ms timer so sleep(1) is 1 ms, not 15.6 ms.
    timeBeginPeriod(1);

    g_stop.store(false);
    g_hook_thread = std::thread(hook_thread_main);
    g_run_thread  = std::thread(run_thread);
}

void Bhop_Shutdown() {
    g_stop.store(true);
    g_suppress.store(false);
    inject_key(false);

    if (g_hook_tid.load())
        PostThreadMessageW(g_hook_tid.load(), WM_QUIT, 0, 0);

    if (g_run_thread.joinable())  g_run_thread.join();
    if (g_hook_thread.joinable()) g_hook_thread.join();

    timeEndPeriod(1);
    g_started.store(false);
}

BhopDebug Bhop_GetDebug() {
    BhopDebug d;
    d.enabled     = g_enabled.load();
    d.scroll      = g_scroll.load();
    d.key_inject  = g_key_inject.load();
    d.on_ground   = g_dbg_ground.load();
    d.focused     = g_dbg_focused.load();
    d.space_held  = g_dbg_space.load();
    d.hook_ok     = g_dbg_hook.load();
    d.driving     = g_dbg_driving.load();
    d.suppressing = g_dbg_suppress.load();
    d.signals     = g_dbg_signals.load();
    d.scrolls     = g_dbg_scrolls.load();
    d.edges       = g_dbg_edges.load();
    return d;
}

void  Bhop_SetEnabled(bool on)      { g_enabled.store(on); if (!on) inject_key(false); }
void  Bhop_SetScroll(bool on)       { g_scroll.store(on); }
void  Bhop_SetKeyInject(bool on)    { g_key_inject.store(on); if (!on) inject_key(false); }
void  Bhop_SetScrollInterval(float ms) {
    if (ms < 2.0f)  ms = 2.0f;
    if (ms > 50.0f) ms = 50.0f;
    g_interval_ms.store(static_cast<double>(ms));
}
float Bhop_ScrollInterval() { return static_cast<float>(g_interval_ms.load()); }
