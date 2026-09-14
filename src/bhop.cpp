// --- src/bhop.cpp ---
// One bhop engine. SPACE only. No memory writes.
//
// Press the key BEFORE the predicted landing (lead), hold it across the
// touchdown so the landing sample sees it down, then fall back to retrying
// while grounded if the prediction was wrong.
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

// Above this the fall is too slow to be a real landing, so we don't predict.
constexpr float kMinFallSpeed = 50.0f;   // units/sec

std::atomic<bool>   g_stop{false}, g_started{false};
std::atomic<bool>   g_enabled{false};
std::atomic<double> g_lead_ms{8.0};
std::atomic<double> g_hold_ms{14.0};
std::atomic<double> g_retry_ms{16.0};

// Diagnostics.
std::atomic<bool>   g_dbg_focus{false}, g_dbg_space{false};
std::atomic<bool>   g_dbg_hook{false},  g_dbg_ground{false};
std::atomic<bool>   g_dbg_suppress{false}, g_dbg_pressing{false};
std::atomic<int>    g_dbg_inj{0}, g_dbg_pred{0};
std::atomic<double> g_dbg_last{0.0};
std::atomic<float>  g_dbg_vz{0.0f}, g_dbg_tti{-1.0f};

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
// produce a new press edge, so our injected edges must be the only ones seen.
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

    bool ground;
    if (g_gw.flag_ok) ground = (fl & 1u) != 0u;   // bitmask, never whole-value
    else              ground = g_gw.z_ground;
    if (g_gw.hge_ok) ground = ground || (hge != 0xFFFFFFFFu);

    g_dbg_ground.store(ground);
    return ground;
}

// ── landing prediction ───────────────────────────────────────────────────
// Velocity from finite differences of Z between DISTINCT samples. Z is written
// once per tick, so sampling on change keeps the estimate clean instead of
// showing stair-steps.
struct Predict {
    bool   have_z = false;
    float  last_z = 0.0f;
    double last_t = 0.0;
    float  vz = 0.0f;          // units/sec, positive = rising
    float  ground_z = 0.0f;    // height last stood at
    bool   have_ground_z = false;
};
Predict g_pr;

void predict_update(float z, bool ground) {
    const double now = now_ms();

    if (!g_pr.have_z) {
        g_pr.have_z = true; g_pr.last_z = z; g_pr.last_t = now;
    }

    if (ground) {
        // Reference height follows slopes and small steps.
        g_pr.ground_z = z;
        g_pr.have_ground_z = true;
        g_pr.vz = 0.0f;
        g_pr.last_z = z;
        g_pr.last_t = now;
        g_dbg_vz.store(0.0f);
        g_dbg_tti.store(-1.0f);
        return;
    }

    if (std::fabs(z - g_pr.last_z) >= 0.05f) {
        const double dt = (now - g_pr.last_t) / 1000.0;
        if (dt > 0.0005 && dt < 0.25) {
            const float v = static_cast<float>((z - g_pr.last_z) / dt);
            g_pr.vz = g_pr.vz * 0.4f + v * 0.6f;
        }
        g_pr.last_z = z;
        g_pr.last_t = now;
    }
    g_dbg_vz.store(g_pr.vz);

    if (!g_pr.have_ground_z || g_pr.vz >= -kMinFallSpeed) {
        g_dbg_tti.store(-1.0f);
        return;
    }

    const float dz = z - g_pr.ground_z;
    if (dz <= 0.0f) { g_dbg_tti.store(0.0f); return; }

    g_dbg_tti.store((dz / -g_pr.vz) * 1000.0f);
}

float predict_tti() { return g_dbg_tti.load(); }

// ── injection: separate down / up so the press is actually sampled ───────
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
    bool      ground = false;

    bool   pressed    = false;
    double press_at   = 0.0;
    double next_press = 0.0;
    bool   armed      = false;   // prediction already fired this airtime

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
            if (pressed) { key_up(); pressed = false; }
            next_press = 0.0;
            armed = false;
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

        // Sample every 1 ms. Prediction needs fine resolution: at 2 ms we lose
        // up to 2 ms of the lead, which is a meaningful chunk of an 8 ms lead.
        const float z = g_mem.read<float>(pawn + offsets::m_vOldOrigin + 8);
        if (now - last_sample >= 1.0) {
            last_sample = now;
            ground = sample_ground(g_mem, pawn);
        }

        if (ground) armed = false;
        predict_update(z, ground);

        const double hold_ms  = g_hold_ms.load();
        const double retry_ms = g_retry_ms.load();
        const float  lead_ms  = static_cast<float>(g_lead_ms.load());

        // ── release as soon as the hold elapses ───────────────────────────
        if (pressed && (now - press_at) >= hold_ms) {
            key_up();
            pressed = false;
        }

        // ── decide whether to start a press ───────────────────────────────
        bool start = false;
        bool by_prediction = false;

        if (ground) {
            // Fallback path: we can see we are grounded and no jump has
            // happened, so press. Retrying is what stops a missed hop from
            // ending the chain.
            if (next_press <= 0.0) next_press = now;
            if (now >= next_press) start = true;
        } else {
            // Prediction path: press early so the key is already down when the
            // landing is sampled.
            next_press = 0.0;   // so the landing retry is immediate
            const float tti = predict_tti();
            if (!armed && tti >= 0.0f && tti <= lead_ms) {
                start = true;
                by_prediction = true;
            }
        }

        if (start && !pressed) {
            key_down();
            pressed  = true;
            press_at = now;

            if (by_prediction) {
                armed = true;
                g_dbg_pred.fetch_add(1);
            } else {
                next_press = now + retry_ms;
            }
            g_dbg_inj.fetch_add(1);
            g_dbg_last.store(now);
        }

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
    d.injected    = g_dbg_inj.load();
    d.pred_hits   = g_dbg_pred.load();
    d.vz          = g_dbg_vz.load();
    d.tti         = g_dbg_tti.load();

    const double last = g_dbg_last.load();
    d.age_ms = (last <= 0.0) ? -1 : static_cast<int>(now_ms() - last);
    return d;
}

void  Bhop_SetEnabled(bool on) { g_enabled.store(on); }
bool  Bhop_Enabled()           { return g_enabled.load(); }

void  Bhop_SetLeadMs(float ms) {
    if (ms < 0.0f)  ms = 0.0f;
    if (ms > 40.0f) ms = 40.0f;
    g_lead_ms.store(static_cast<double>(ms));
}
float Bhop_LeadMs() { return static_cast<float>(g_lead_ms.load()); }

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
