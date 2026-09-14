// --- src/bhop.cpp ---
// Six independent bhop engines. None writes to cs2.exe.
//
// ══ THE LATCH-UP THAT MADE BHOP STOP ════════════════════════════════════
// The key engines used to fire ONCE on the rising edge of the ground flag:
//
//     if (ground && !prev_ground) inject();      // <-- the bug
//     prev_ground = ground;
//
// A missed hop leaves you standing on the ground with the flag stuck TRUE, so
// prev_ground stays true, no rising edge ever occurs again, and the engine
// never injects until the gate is released and re-pressed. ONE missed hop ended
// the chain permanently.
//
// Every key engine now retries while grounded (see key_engine_step), so a miss
// costs one tick and the next attempt happens automatically.
#include "bhop.h"
#include "memory.h"
#include "offsets.h"

#include <Windows.h>
#include <mmsystem.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <thread>

#pragma comment(lib, "winmm.lib")

extern HWND g_cs2_hwnd;

namespace {

// One client tick at 64 tick. Used as the retry cadence and the KEY_EDGE_DEL
// delay -- 15.625 ms exactly, because 15.0 ms measurably missed more hops.
constexpr double kTickMs = 15.625;

std::atomic<bool>   g_stop{false}, g_started{false};
std::atomic<bool>   g_engine[BHOP_ENGINE_COUNT];
std::atomic<double> g_scroll_ms{8.0};
std::atomic<double> g_repeat_ms{kTickMs};
std::atomic<double> g_delay_ms{kTickMs};
std::atomic<int>    g_inject_key{VK_F20};
std::atomic<int>    g_fps_target{64};

// Diagnostics.
std::atomic<bool> g_dbg_focus{false}, g_dbg_space{false};
std::atomic<bool> g_dbg_hook{false},  g_dbg_ground{false};
std::atomic<bool> g_dbg_suppress{false};
std::atomic<int>  g_dbg_signals{0};
std::atomic<int>  g_eng_inj[BHOP_ENGINE_COUNT];
std::atomic<bool> g_eng_active[BHOP_ENGINE_COUNT];
std::atomic<double> g_eng_last[BHOP_ENGINE_COUNT];
std::atomic<bool> g_fps_sent{false};

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
// are ignored) and swallows it while any engine is driving, because a held
// +jump cannot produce a new press edge.
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

    // BITMASK on the flag, never a whole-value comparison.
    bool ground;
    if (g_gw.flag_ok) ground = (fl & 1u) != 0u;
    else              ground = g_gw.z_ground;
    if (g_gw.hge_ok) ground = ground || (hge != 0xFFFFFFFFu);

    g_dbg_ground.store(ground);
    return ground;
}

// ── injection primitives ─────────────────────────────────────────────────

void wheel_sendinput() {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    in.mi.mouseData = static_cast<DWORD>(-WHEEL_DELTA);
    SendInput(1, &in, sizeof(INPUT));
}

void wheel_mouse_event() {
    mouse_event(MOUSEEVENTF_WHEEL, 0, 0,
                static_cast<DWORD>(-WHEEL_DELTA), 0);
}

// A key down+up pair. dwExtraInfo and time are zeroed because that is what the
// working UnknownCheats implementation does.
void key_pair(int vk) {
    INPUT in[2]{};
    in[0].type = INPUT_KEYBOARD;
    in[0].ki.wVk = static_cast<WORD>(vk);
    in[0].ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
    in[0].ki.dwExtraInfo = 0;
    in[0].ki.time = 0;

    in[1] = in[0];
    in[1].ki.dwFlags = KEYEVENTF_KEYUP;

    SendInput(2, in, sizeof(INPUT));
}

void tap_key(int vk) {
    INPUT in[2]{};
    for (int i = 0; i < 2; ++i) {
        in[i].type = INPUT_KEYBOARD;
        in[i].ki.wVk = static_cast<WORD>(vk);
        in[i].ki.wScan = static_cast<WORD>(MapVirtualKeyW(vk, MAPVK_VK_TO_VSC));
        if (i == 1) in[i].ki.dwFlags = KEYEVENTF_KEYUP;
    }
    SendInput(2, in, sizeof(INPUT));
}

// ── console command (FPS64) ──────────────────────────────────────────────
bool send_console_command(const char* cmd) {
    if (!g_cs2_hwnd || !IsWindow(g_cs2_hwnd)) return false;

    tap_key(VK_OEM_3);
    wait_ms(80.0);

    for (const char* p = cmd; *p; ++p) {
        const SHORT vk = VkKeyScanA(*p);
        if (vk == -1) continue;

        const int  key   = vk & 0xFF;
        const bool shift = (vk >> 8) & 1;

        INPUT in[4]{};
        int n = 0;
        if (shift) {
            in[n].type = INPUT_KEYBOARD;
            in[n].ki.wVk = VK_SHIFT;
            ++n;
        }
        in[n].type = INPUT_KEYBOARD;
        in[n].ki.wVk = static_cast<WORD>(key);
        ++n;
        in[n].type = INPUT_KEYBOARD;
        in[n].ki.wVk = static_cast<WORD>(key);
        in[n].ki.dwFlags = KEYEVENTF_KEYUP;
        ++n;
        if (shift) {
            in[n].type = INPUT_KEYBOARD;
            in[n].ki.wVk = VK_SHIFT;
            in[n].ki.dwFlags = KEYEVENTF_KEYUP;
            ++n;
        }
        SendInput(n, in, sizeof(INPUT));
        wait_ms(8.0);
    }

    wait_ms(40.0);
    tap_key(VK_RETURN);
    wait_ms(50.0);
    tap_key(VK_OEM_3);
    return true;
}

// ── engines ──────────────────────────────────────────────────────────────
// Each engine is a separate struct with its own timers. Nothing about one
// leaks into another.
struct Engine {
    bool   prev_gnd = false;
    double next_out = 0.0;

    void reset() { prev_gnd = false; next_out = 0.0; }
};

void mark(int engine, double now) {
    g_eng_inj[engine].fetch_add(1);
    g_eng_last[engine].store(now);
}

// KEY_EDGE / KEY_EDGE_DEL / FPS64.
//
// *** THIS IS THE LATCH FIX ***
// While grounded it keeps injecting every `repeat_ms`. Standing on the ground
// (because a hop was missed) no longer silences the engine -- it simply retries
// until a jump takes. `use_delay` postpones only the FIRST pair of each grounded
// period, which preserves the one-tick-delay behaviour that was working.
void key_engine_step(Engine& e, int engine, bool ground, bool use_delay,
                     double delay_ms, double repeat_ms, int vk) {
    if (!ground) { e.prev_gnd = false; return; }

    const double now = now_ms();

    if (!e.prev_gnd) {
        e.prev_gnd = true;
        e.next_out = now + (use_delay ? delay_ms : 0.0);
    }

    if (now >= e.next_out) {
        key_pair(vk);
        mark(engine, now);
        e.next_out = now + repeat_ms;
    }
}

// KEY_REPEAT: no ground dependency at all, so there is no state machine that
// can latch. It injects for as long as the gate is held. While airborne the
// presses rely on the game's jump buffering.
void repeat_engine_step(Engine& e, int engine, double repeat_ms, int vk) {
    const double now = now_ms();
    if (e.next_out <= 0.0) e.next_out = now;   // fire immediately on engage

    if (now >= e.next_out) {
        key_pair(vk);
        mark(engine, now);
        e.next_out = now + repeat_ms;
    }
}

void run_thread() {
    uintptr_t pawn = 0;
    double    pawn_at = 0.0, last_sample = 0.0;
    bool      ground = false;
    Engine    eng[BHOP_ENGINE_COUNT];

    while (!g_stop.load()) {
        wait_ms(1.0);
        if (!g_mem.is_valid()) continue;

        const bool focused = cs2_focused();
        const bool space   = space_held();
        g_dbg_focus.store(focused);
        g_dbg_space.store(space);

        bool any_on = false;
        for (int i = 0; i < BHOP_ENGINE_COUNT; ++i)
            if (g_engine[i].load()) { any_on = true; break; }

        const bool driving = any_on && focused && space;
        g_suppress.store(driving);
        g_dbg_suppress.store(driving);

        if (!driving) {
            for (int i = 0; i < BHOP_ENGINE_COUNT; ++i) {
                eng[i].reset();
                g_eng_active[i].store(false);
            }
            continue;
        }

        const double now = now_ms();
        if (!pawn || now - pawn_at > 500.0) {
            pawn = g_mem.read<uintptr_t>(
                g_mem.client_dll + offsets::dwLocalPlayerPawn);
            pawn_at = now;
        }
        if (!pawn) continue;

        if (now - last_sample >= 2.0) {
            last_sample = now;
            ground = sample_ground(g_mem, pawn);
        }

        const double repeat_ms = g_repeat_ms.load();
        const int    vk        = g_inject_key.load();

        // ── 1. SCROLL_SI ────────────────────────────────────────────────
        if (g_engine[BHOP_SCROLL_SI].load()) {
            Engine& e = eng[BHOP_SCROLL_SI];
            if (now >= e.next_out) {
                e.next_out = now + g_scroll_ms.load();
                wheel_sendinput();
                mark(BHOP_SCROLL_SI, now);
            }
            g_eng_active[BHOP_SCROLL_SI].store(true);
        } else g_eng_active[BHOP_SCROLL_SI].store(false);

        // ── 2. KEY_EDGE ─────────────────────────────────────────────────
        if (g_engine[BHOP_KEY_EDGE].load()) {
            key_engine_step(eng[BHOP_KEY_EDGE], BHOP_KEY_EDGE, ground, false,
                            0.0, repeat_ms, vk);
            g_eng_active[BHOP_KEY_EDGE].store(true);
        } else g_eng_active[BHOP_KEY_EDGE].store(false);

        // ── 3. KEY_EDGE_DEL ─────────────────────────────────────────────
        if (g_engine[BHOP_KEY_EDGE_DEL].load()) {
            key_engine_step(eng[BHOP_KEY_EDGE_DEL], BHOP_KEY_EDGE_DEL, ground,
                            true, g_delay_ms.load(), repeat_ms, vk);
            g_eng_active[BHOP_KEY_EDGE_DEL].store(true);
        } else g_eng_active[BHOP_KEY_EDGE_DEL].store(false);

        // ── 4. SCROLL_ME ────────────────────────────────────────────────
        if (g_engine[BHOP_SCROLL_ME].load()) {
            Engine& e = eng[BHOP_SCROLL_ME];
            if (now >= e.next_out) {
                e.next_out = now + g_scroll_ms.load();
                wheel_mouse_event();
                mark(BHOP_SCROLL_ME, now);
            }
            g_eng_active[BHOP_SCROLL_ME].store(true);
        } else g_eng_active[BHOP_SCROLL_ME].store(false);

        // ── 5. FPS64 ────────────────────────────────────────────────────
        if (g_engine[BHOP_FPS64].load()) {
            if (!g_fps_sent.load()) {
                char cmd[48];
                std::snprintf(cmd, sizeof(cmd), "fps_max %d", g_fps_target.load());
                if (send_console_command(cmd)) g_fps_sent.store(true);
            }
            key_engine_step(eng[BHOP_FPS64], BHOP_FPS64, ground, true,
                            g_delay_ms.load(), repeat_ms, vk);
            g_eng_active[BHOP_FPS64].store(true);
        } else g_eng_active[BHOP_FPS64].store(false);

        // ── 6. KEY_REPEAT ───────────────────────────────────────────────
        if (g_engine[BHOP_KEY_REPEAT].load()) {
            repeat_engine_step(eng[BHOP_KEY_REPEAT], BHOP_KEY_REPEAT,
                               repeat_ms, vk);
            g_eng_active[BHOP_KEY_REPEAT].store(true);
        } else g_eng_active[BHOP_KEY_REPEAT].store(false);
    }

    if (g_fps_sent.load()) {
        g_suppress.store(false);
        send_console_command("fps_max 0");
    }
    g_suppress.store(false);
}

} // namespace

void Bhop_Init() {
    if (!g_mem.is_valid()) return;
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;

    timeBeginPeriod(1);

    for (int i = 0; i < BHOP_ENGINE_COUNT; ++i) {
        g_engine[i].store(false);
        g_eng_last[i].store(-1.0);
    }

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
    d.fps_cmd_sent = g_fps_sent.load();
    d.fps_target   = g_fps_target.load();

    const double now = now_ms();
    for (int i = 0; i < BHOP_ENGINE_COUNT; ++i) {
        d.active[i]   = g_eng_active[i].load();
        d.injected[i] = g_eng_inj[i].load();
        const double last = g_eng_last[i].load();
        d.age_ms[i] = (last < 0.0)
                        ? -1
                        : static_cast<int>(now - last);
    }
    return d;
}

void Bhop_SetEngine(int engine, bool on) {
    if (engine < 0 || engine >= BHOP_ENGINE_COUNT) return;
    g_engine[engine].store(on);
    if (engine == BHOP_FPS64 && !on && g_fps_sent.load() && g_cs2_hwnd) {
        g_fps_sent.store(false);
        g_suppress.store(false);
        send_console_command("fps_max 0");
    }
}

bool Bhop_GetEngine(int engine) {
    if (engine < 0 || engine >= BHOP_ENGINE_COUNT) return false;
    return g_engine[engine].load();
}

void  Bhop_SetScrollInterval(float ms) {
    if (ms < 2.0f)  ms = 2.0f;
    if (ms > 40.0f) ms = 40.0f;
    g_scroll_ms.store(static_cast<double>(ms));
}
float Bhop_ScrollInterval() { return static_cast<float>(g_scroll_ms.load()); }

void  Bhop_SetRepeatMs(float ms) {
    if (ms < 4.0f)  ms = 4.0f;
    if (ms > 60.0f) ms = 60.0f;
    g_repeat_ms.store(static_cast<double>(ms));
}
float Bhop_RepeatMs() { return static_cast<float>(g_repeat_ms.load()); }

void  Bhop_SetDelayMs(float ms) {
    if (ms < 0.0f)  ms = 0.0f;
    if (ms > 50.0f) ms = 50.0f;
    g_delay_ms.store(static_cast<double>(ms));
}
float Bhop_DelayMs() { return static_cast<float>(g_delay_ms.load()); }

void Bhop_SetInjectKey(int vk) { g_inject_key.store(vk); }
int  Bhop_InjectKey()          { return g_inject_key.load(); }

void Bhop_SetFpsTarget(int fps) {
    if (fps < 32)  fps = 32;
    if (fps > 300) fps = 300;
    g_fps_target.store(fps);
    g_fps_sent.store(false);
}
int Bhop_FpsTarget() { return g_fps_target.load(); }
