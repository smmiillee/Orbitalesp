// --- src/bhop.cpp ---
// Bhop by KEYSTROKE INJECTION ONLY. There is no memory write anywhere in this
// file: the project no longer writes to cs2.exe at all.
//
//   the injected key carries "+jump", and a jump needs a fresh PRESS edge, so
//   what matters is generating exactly one clean edge per landing.
//
// ── WHY YOUR HELD KEY IS SWALLOWED ───────────────────────────────────────
// Holding space makes the engine's own +jump stay down, and a held button
// cannot produce a new jump. So while bhop is driving, the low-level hook
// swallows your physical spacebar and the game's jump input comes only from us.
// Your held key becomes a gate; the edges come from here.
//
// ── THE TWO STRATEGIES ───────────────────────────────────────────────────
// PLAIN: press when the ground flag reads grounded. The flag is written once
// per tick (~15.6 ms), so detection can arrive after the landing tick.
//
// PREDICTIVE: estimate the landing from vertical velocity and press early, so
// the press is already down when the engine samples input for that tick.
//   tti = (z - ground_z) / (-vz)        time until the predicted impact
//   press when tti <= lead              (lead defaults to one tick, ~16 ms)
// ground_z is the height you were last standing at, and vz comes from finite
// differences of Z between DISTINCT samples -- sampling only on change keeps the
// velocity clean, since the game only writes Z once per tick.
//
// The observed ground flag always remains a fallback, so prediction can only
// add presses, never remove them.
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

// Fallback lead, one tick at 64 Hz. Overridable from the menu.
constexpr float kDefaultLeadMs = 16.0f;

// How long a press is held once started. Long enough to span the landing tick,
// short enough that the next landing still gets a fresh edge.
constexpr double kPressHoldMs = 30.0;

std::atomic<bool> g_stop{false}, g_started{false};
std::atomic<bool> g_enabled{true};
std::atomic<bool> g_predict{false};
std::atomic<bool> g_separate_key{false};
std::atomic<float> g_lead_ms{kDefaultLeadMs};

// Diagnostics.
std::atomic<bool>  g_dbg_ground{false}, g_dbg_focused{false};
std::atomic<bool>  g_dbg_space{false},  g_dbg_driving{false};
std::atomic<bool>  g_dbg_suppress{false};
std::atomic<int>   g_dbg_signals{0},    g_dbg_edges{0}, g_dbg_predicted{0};
std::atomic<float> g_dbg_vz{0.0f},      g_dbg_tti{-1.0f};

// Keyboard hook.
std::atomic<bool>  g_phys_space{false};   // physical (non-injected) spacebar
std::atomic<bool>  g_hook_ok{false};
std::atomic<bool>  g_suppress{false};     // swallow the physical spacebar
std::atomic<DWORD> g_hook_tid{0};
HHOOK g_hook = nullptr;

std::thread g_bhop_thread, g_hook_thread;

double now_ms() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0).count();
}

// ~1 ms sleep that actually works. timeBeginPeriod(1) is what makes this honest:
// without it Windows quantises sleeps to ~15.6 ms, which is a whole game tick.
// The spin covers the last fraction either way.
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
//      ours are ignored -- the only way to read your real key while injecting.
//   2. while bhop is driving, swallow the physical spacebar so the engine's
//      +jump cannot stay held and block our press edges.
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
    g_hook_ok.store(g_hook != nullptr);

    // A low-level hook is delivered on the thread that installed it, so this
    // must pump messages or the callback never runs.
    MSG msg{};
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }

    if (g_hook) { UnhookWindowsHookEx(g_hook); g_hook = nullptr; }
    g_hook_ok.store(false);
}

bool space_held() {
    if (g_hook_ok.load()) return g_phys_space.load();
    return (GetAsyncKeyState(VK_SPACE) & 0x8000) != 0;
}

// ── ground state ─────────────────────────────────────────────────────────
// Z comes from m_vOldOrigin, which is verified working. m_fFlags and
// m_hGroundEntity are not verified for this build, so each is only trusted
// after it has been seen set while Z is static AND clear while Z is moving --
// a wrong offset then degrades the verdict instead of breaking it.
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
    if (g_gw.flag_ok) ground = (fl & 1u) != 0u;   // instance-accurate
    else              ground = g_gw.z_ground;     // needs a 22 ms window
    if (g_gw.hge_ok) ground = ground || (hge != 0xFFFFFFFFu);

    g_dbg_ground.store(ground);
    return ground;
}

// ── landing prediction ───────────────────────────────────────────────────
// Velocity is taken from finite differences of Z between DISTINCT samples.
// The game only writes Z once per tick, so sampling on change keeps vz clean
// instead of showing the stair-steps you would get from polling faster.
struct Predict {
    bool   have_z = false;
    float  last_z = 0.0f;
    double last_t = 0.0;
    float  vz = 0.0f;           // units/sec, positive = rising
    float  ground_z = 0.0f;     // height last stood at
    bool   have_ground_z = false;
    bool   armed = false;       // already pressed for this airtime
};
Predict g_pr;

// Returns true when we should press NOW because a landing is imminent.
bool predict_step(const Memory& mem, uintptr_t pawn, float z, bool ground,
                  float lead_ms) {
    const double now = now_ms();

    if (ground) {
        // Refresh the reference height while standing, so slopes and small
        // steps are tracked. Re-arming here is what allows the next airtime to
        // press again.
        g_pr.ground_z = z;
        g_pr.have_ground_z = true;
        g_pr.armed = false;
        g_pr.vz = 0.0f;
        g_pr.last_z = z;
        g_pr.last_t = now;
        g_pr.have_z = true;
        g_dbg_vz.store(0.0f);
        g_dbg_tti.store(-1.0f);
        return false;
    }

    if (!g_pr.have_z) {
        g_pr.have_z = true;
        g_pr.last_z = z;
        g_pr.last_t = now;
        return false;
    }

    // Only recompute when Z actually moved, i.e. when a new tick's value
    // arrived. Otherwise dt would shrink without new information and the
    // velocity would spike.
    if (std::fabs(z - g_pr.last_z) >= 0.05f) {
        const double dt = (now - g_pr.last_t) / 1000.0;
        if (dt > 0.0005 && dt < 0.25) {
            const float v = static_cast<float>((z - g_pr.last_z) / dt);
            // Light smoothing: enough to steady the estimate, not enough to lag
            // a real landing.
            g_pr.vz = g_pr.vz * 0.5f + v * 0.5f;
        }
        g_pr.last_z = z;
        g_pr.last_t = now;
    }

    g_dbg_vz.store(g_pr.vz);

    if (!g_pr.have_ground_z || g_pr.vz >= -1.0f) {
        g_dbg_tti.store(-1.0f);
        return false;
    }

    // Falling towards the height we last stood at.
    const float dz = z - g_pr.ground_z;
    if (dz <= 0.0f) {
        // Already at or below the reference height -- landing is now.
        g_dbg_tti.store(0.0f);
        return true;
    }

    const float tti_ms = (dz / -g_pr.vz) * 1000.0f;
    g_dbg_tti.store(tti_ms);

    if (g_pr.armed) return false;          // one press per airtime
    if (tti_ms <= lead_ms) {
        g_pr.armed = true;
        return true;
    }
    return false;
}

// ── output ───────────────────────────────────────────────────────────────
// +jump is bound to SPACE by default. The separate-key option injects RIGHT
// instead, for cases where re-injecting the key you are physically holding
// behaves oddly. Either way the physical key stays swallowed, because a held
// +jump cannot produce a new jump.
int inject_vk() {
    return g_separate_key.load() ? VK_RIGHT : VK_SPACE;
}

void inject(bool down) {
    static bool s_down = false;
    if (down == s_down) return;
    s_down = down;

    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = static_cast<WORD>(inject_vk());
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(inject_vk(), MAPVK_VK_TO_VSC));
    in.ki.dwFlags = down ? 0u : KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(INPUT));
}

void run_thread() {
    uintptr_t pawn = 0;
    double    pawn_at = 0.0, last_sample = 0.0;
    bool      ground = false;
    bool      out_down = false;
    double    press_until = 0.0;

    while (!g_stop.load()) {
        wait_ms(1);
        if (!g_mem.is_valid()) continue;

        const bool focused = cs2_focused();
        const bool space   = space_held();
        const bool active  = g_enabled.load() && focused && space;

        g_dbg_focused.store(focused);
        g_dbg_space.store(space);

        // Swallow your physical key only while we are actually driving, so
        // space behaves normally everywhere else.
        const bool suppress = active;
        g_suppress.store(suppress);
        g_dbg_suppress.store(suppress);

        if (!active) {
            if (out_down) { inject(false); out_down = false; }
            press_until = 0.0;
            g_pr.armed = false;
            g_dbg_driving.store(false);
            continue;
        }

        const double now = now_ms();
        if (!pawn || now - pawn_at > 500.0) {
            pawn = g_mem.read<uintptr_t>(
                g_mem.client_dll + offsets::dwLocalPlayerPawn);
            pawn_at = now;
        }
        if (!pawn) { if (out_down) { inject(false); out_down = false; } continue; }

        // 2 ms sampling. Must be comfortably faster than a 15.6 ms tick for
        // prediction to have any resolution at all.
        const float z = g_mem.read<float>(pawn + offsets::m_vOldOrigin + 8);
        bool predicted_now = false;

        if (now - last_sample >= 2.0) {
            last_sample = now;
            ground = sample_ground(g_mem, pawn);
        }

        // ── PREDICTION ───────────────────────────────────────────────────
        if (g_predict.load()) {
            predicted_now = predict_step(g_mem, pawn, z, ground,
                                         g_lead_ms.load());
        } else {
            g_dbg_tti.store(-1.0f);
            g_pr.armed = false;
            // Keep the reference height fresh even when prediction is off, so
            // enabling it later does not need a warm-up.
            if (ground) { g_pr.ground_z = z; g_pr.have_ground_z = true; }
        }

        // ── PRESS DECISIONS ──────────────────────────────────────────────
        // A press window is opened by either a predicted imminent landing or by
        // actually observing the ground flag. The window is what guarantees the
        // press is still down when the engine samples input for that tick.
        bool start = false;
        if (predicted_now) { start = true; g_dbg_predicted.fetch_add(1); }
        if (ground)        { start = true; }

        if (start && now >= press_until)
            press_until = now + kPressHoldMs;

        const bool want = now < press_until;

        if (want != out_down) {
            inject(want);
            if (want) g_dbg_edges.fetch_add(1);
        }
        out_down = want;
        g_dbg_driving.store(true);
    }

    g_suppress.store(false);
    inject(false);
}

} // namespace

void Bhop_Init() {
    if (!g_mem.is_valid()) return;
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;

    // Ask Windows for a 1 ms timer so sleep(1) is 1 ms and not 15.6 ms. This is
    // the one genuinely useful thing in the cs2-bhop reference.
    timeBeginPeriod(1);

    g_stop.store(false);
    g_hook_thread = std::thread(hook_thread_main);
    g_bhop_thread = std::thread(run_thread);
}

void Bhop_Shutdown() {
    g_stop.store(true);
    g_suppress.store(false);
    inject(false);

    if (g_hook_tid.load())
        PostThreadMessageW(g_hook_tid.load(), WM_QUIT, 0, 0);

    if (g_bhop_thread.joinable()) g_bhop_thread.join();
    if (g_hook_thread.joinable()) g_hook_thread.join();

    timeEndPeriod(1);
    g_started.store(false);
}

BhopDebug Bhop_GetDebug() {
    BhopDebug d;
    d.enabled      = g_enabled.load();
    d.predict      = g_predict.load();
    d.separate_key = g_separate_key.load();
    d.on_ground    = g_dbg_ground.load();
    d.focused      = g_dbg_focused.load();
    d.space_held   = g_dbg_space.load();
    d.hook_ok      = g_hook_ok.load();
    d.driving      = g_dbg_driving.load();
    d.suppressing  = g_dbg_suppress.load();
    d.signals      = g_dbg_signals.load();
    d.edges        = g_dbg_edges.load();
    d.predicted    = g_dbg_predicted.load();
    d.vz           = g_dbg_vz.load();
    d.tti          = g_dbg_tti.load();
    return d;
}

void  Bhop_SetEnabled(bool on)     { g_enabled.store(on); if (!on) inject(false); }
void  Bhop_SetPredict(bool on)     { g_predict.store(on); }
void  Bhop_SetSeparateKey(bool on) { g_separate_key.store(on); inject(false); }
void  Bhop_SetLead(float ms)       { g_lead_ms.store(ms); }
float Bhop_Lead()                  { return g_lead_ms.load(); }
