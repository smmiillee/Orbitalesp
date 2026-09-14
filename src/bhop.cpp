// --- src/bhop.cpp ---
// Five independent bhop engines. No engine writes to cs2.exe.
//
// ══ WHAT IS SHARED, AND WHY THAT IS NOT "COUPLING" ═══════════════════════
// The five engines share only READ-ONLY infrastructure:
//   * keyboard hook        - tracks the physical spacebar and swallows it
//   * ground sampling      - one m_fFlags read per loop
//   * focus / gate check   - "is CS2 foreground and is space held"
// Each engine's DECISIONS and OUTPUT are entirely its own. Disabling one
// cannot affect another, which is what was asked for.
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
#include <vector>

#pragma comment(lib, "winmm.lib")

extern HWND g_cs2_hwnd;

namespace {

// One client tick at 64 tick: the correct delay for KEY_SI_DEL, and the frame
// budget FPS64 tries to align to. 15625 us exactly, not 15 ms -- the UC thread
// found 15.625 ms missed noticeably fewer hops than 15.0 ms.
constexpr double kTickMs  = 15.625;
constexpr double kPressMs = 30.0;

std::atomic<bool>   g_stop{false}, g_started{false};
std::atomic<bool>   g_engine[BHOP_ENGINE_COUNT];
std::atomic<double> g_scroll_ms{8.0};
std::atomic<double> g_delay_ms{kTickMs};
std::atomic<int>    g_inject_key{VK_F20};
std::atomic<int>    g_fps_target{64};

// Shared diagnostics.
std::atomic<bool> g_dbg_focus{false}, g_dbg_space{false};
std::atomic<bool> g_dbg_hook{false},  g_dbg_ground{false};
std::atomic<bool> g_dbg_suppress{false};
std::atomic<int>  g_dbg_signals{0};
std::atomic<int>  g_eng_inj[BHOP_ENGINE_COUNT];
std::atomic<bool> g_eng_active[BHOP_ENGINE_COUNT];
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

// ~1 ms sleep that actually works. timeBeginPeriod(1) makes it honest; the spin
// covers the last fraction.
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

// m_hGroundEntity is not verified for this build, so it is graded before use.
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

    // BITMASK on the flag, never a whole-value comparison. The flag is
    // instance-accurate once graded, which matters because the grounded frame
    // can be a single tick.
    bool ground;
    if (g_gw.flag_ok) ground = (fl & 1u) != 0u;
    else              ground = g_gw.z_ground;
    if (g_gw.hge_ok) ground = ground || (hge != 0xFFFFFFFFu);

    g_dbg_ground.store(ground);
    return ground;
}

// ── raw injection primitives ─────────────────────────────────────────────

// One wheel notch via SendInput.
void wheel_sendinput() {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_WHEEL;
    in.mi.mouseData = static_cast<DWORD>(-WHEEL_DELTA);
    SendInput(1, &in, sizeof(INPUT));
}

// One wheel notch via the legacy mouse_event API. Different path through the
// input stack, which is the entire point of having it as an option.
void wheel_mouse_event() {
    mouse_event(MOUSEEVENTF_WHEEL, 0, 0,
                static_cast<DWORD>(-WHEEL_DELTA), 0);
}

// A key down+up pair. dwExtraInfo and time are explicitly zeroed because that
// is what the working UnknownCheats implementation does.
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

// ── console command injection (FPS64) ────────────────────────────────────
// Opens the console with the default ` key, types the command, presses enter
// and closes it again. This is keystroke injection, not memory access.
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

bool send_console_command(const char* cmd) {
    if (!g_cs2_hwnd || !IsWindow(g_cs2_hwnd)) return false;

    tap_key(VK_OEM_3);      // ` opens the console
    wait_ms(80.0);

    for (const char* p = cmd; *p; ++p) {
        const SHORT vk = VkKeyScanA(*p);
        if (vk == -1) continue;

        const int key   = vk & 0xFF;
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
    tap_key(VK_OEM_3);      // close it
    return true;
}

// ── per-engine state ─────────────────────────────────────────────────────
// Each engine is a separate struct with its own timers, so nothing about one
// leaks into another.
struct Engine {
    bool   active   = false;
    bool   armed    = false;    // already pressed for this airtime
    bool   pending  = false;    // KEY_SI_DEL: pair scheduled
    double fire_at  = 0.0;
    double next_out = 0.0;
    bool   prev_gnd = false;

    void reset() { armed = false; pending = false; fire_at = 0.0;
                   next_out = 0.0; prev_gnd = false; }
};

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

        // Does ANY engine want to drive right now? Only then do we swallow the
        // physical spacebar, so space behaves normally when all engines are off.
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

        // Shared read, per-engine decisions. 2 ms sampling keeps resolution
        // well inside a 15.6 ms tick.
        if (now - last_sample >= 2.0) {
            last_sample = now;
            ground = sample_ground(g_mem, pawn);
        }

        // ── SCROLL_SI : continuous wheel spam ────────────────────────────
        if (g_engine[BHOP_SCROLL_SI].load()) {
            Engine& e = eng[BHOP_SCROLL_SI];
            e.active = true;
            if (now >= e.next_out) {
                e.next_out = now + g_scroll_ms.load();
                wheel_sendinput();
                g_eng_inj[BHOP_SCROLL_SI].fetch_add(1);
            }
        } else { eng[BHOP_SCROLL_SI].active = false; }

        // ── SCROLL_ME : same, older API ──────────────────────────────────
        if (g_engine[BHOP_SCROLL_ME].load()) {
            Engine& e = eng[BHOP_SCROLL_ME];
            e.active = true;
            if (now >= e.next_out) {
                e.next_out = now + g_scroll_ms.load();
                wheel_mouse_event();
                g_eng_inj[BHOP_SCROLL_ME].fetch_add(1);
            }
        } else { eng[BHOP_SCROLL_ME].active = false; }

        // ── KEY_SI : one pair on the rising edge of grounded ─────────────
        if (g_engine[BHOP_KEY_SI].load()) {
            Engine& e = eng[BHOP_KEY_SI];
            e.active = true;
            if (ground && !e.prev_gnd) {
                key_pair(g_inject_key.load());
                g_eng_inj[BHOP_KEY_SI].fetch_add(1);
            }
            e.prev_gnd = ground;
        } else { eng[BHOP_KEY_SI].active = false; }

        // ── KEY_SI_DEL : same, but one tick after the landing appears ────
        if (g_engine[BHOP_KEY_SI_DEL].load()) {
            Engine& e = eng[BHOP_KEY_SI_DEL];
            e.active = true;

            if (ground && !e.prev_gnd && !e.pending) {
                e.pending = true;
                e.fire_at = now + g_delay_ms.load();
            }
            if (e.pending && now >= e.fire_at) {
                key_pair(g_inject_key.load());
                g_eng_inj[BHOP_KEY_SI_DEL].fetch_add(1);
                e.pending = false;
            }
            e.prev_gnd = ground;
        } else { eng[BHOP_KEY_SI_DEL].active = false; }

        // ── FPS64 : send fps_max once, then tick-aligned key pairs ───────
        if (g_engine[BHOP_FPS64].load()) {
            Engine& e = eng[BHOP_FPS64];
            e.active = true;

            if (!g_fps_sent.load()) {
                char cmd[48];
                std::snprintf(cmd, sizeof(cmd), "fps_max %d", g_fps_target.load());
                if (send_console_command(cmd)) {
                    g_fps_sent.store(true);
                    g_eng_inj[BHOP_FPS64].fetch_add(1);
                }
            }
            if (ground && !e.prev_gnd) {
                key_pair(g_inject_key.load());
                g_eng_inj[BHOP_FPS64].fetch_add(1);
            }
            e.prev_gnd = ground;
        } else { eng[BHOP_FPS64].active = false; }

        for (int i = 0; i < BHOP_ENGINE_COUNT; ++i)
            g_eng_active[i].store(eng[i].active);
    }

    // If FPS64 was left on, put the framerate cap back to the game default.
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

    for (int i = 0; i < BHOP_ENGINE_COUNT; ++i) g_engine[i].store(false);

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
    for (int i = 0; i < BHOP_ENGINE_COUNT; ++i) {
        d.active[i]   = g_eng_active[i].load();
        d.injected[i] = g_eng_inj[i].load();
    }
    return d;
}

void Bhop_SetEngine(int engine, bool on) {
    if (engine < 0 || engine >= BHOP_ENGINE_COUNT) return;
    g_engine[engine].store(on);
    // Turning FPS64 off must restore the cap rather than leaving the game at 64.
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
    g_fps_sent.store(false);   // re-send on the next tick
}
int Bhop_FpsTarget() { return g_fps_target.load(); }
