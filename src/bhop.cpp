// --- src/bhop.cpp ---
// Bhop. SPACE only. No memory writes.
#include "bhop.h"
#include "memory.h"
#include "offsets.h"

#include <Windows.h>
#include <mmsystem.h>
#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

#pragma comment(lib, "winmm.lib")

extern HWND g_cs2_hwnd;

namespace {

// ══ LOCKED TIMING, AND WHY EACH VALUE ═══════════════════════════════════

// Lead time before the predicted landing.
//   Needs to cover: our detection lag (one sample, ~1-2 ms) + SendInput
//   latency. Must stay under about half a tick (~7 ms at 64 tick) or the press
//   starts firing uselessly while still airborne. 8 ms sits between those.
constexpr double kLeadMs = 8.0;

// How long the key is held down.
//   The press must span at least one frame or the game's per-frame input sample
//   never sees it -- except CS2's input system processes queued key events, so a
//   shorter press is not automatically invisible. 12 ms spans a frame at up to
//   ~83 fps and gives ~2 frame samples per press at 144 fps, which is the
//   margin that matters. Also comfortably longer than the fallback interval's
//   release gap, so every retry is a distinct edge.
constexpr double kHoldMs = 12.0;

// Retry interval while grounded, when prediction missed.
//   One 64-tick interval. Matching the tick avoids beating against it, which is
//   what produced the long runs of hits followed by long runs of misses.
constexpr double kRetryMs = 16.0;

// Below this the fall is too slow to be a real landing, so no prediction.
constexpr float kMinFallSpeed = 50.0f;   // units/sec

// Above this the fall has barely started; avoid firing off a tiny drop.
constexpr float kMaxTtiMs = 400.0f;

std::atomic<bool> g_stop{false}, g_started{false}, g_enabled{false};
std::atomic<bool> g_dbg_focus{false}, g_dbg_space{false};
std::atomic<bool> g_dbg_hook{false},  g_dbg_ground{false};
std::atomic<bool> g_dbg_pressing{false};

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

// Tracks the PHYSICAL spacebar (injected events carry LLKHF_INJECTED, so ours
// are ignored) and swallows it while driving, because a held +jump cannot
// produce a new press edge.
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

    // Bitmask on the flag, never a whole-value comparison.
    bool ground;
    if (g_gw.flag_ok) ground = (fl & 1u) != 0u;
    else              ground = g_gw.z_ground;
    if (g_gw.hge_ok) ground = ground || (hge != 0xFFFFFFFFu);

    g_dbg_ground.store(ground);
    return ground;
}

// ── landing prediction ───────────────────────────────────────────────────
struct Predict {
    bool   have_z = false;
    float  last_z = 0.0f;
    double last_t = 0.0;
    float  vz = 0.0f;
    float  ground_z = 0.0f;
    bool   have_ground_z = false;
    float  tti = -1.0f;
};
Predict g_pr;

void predict_update(float z, bool ground) {
    const double now = now_ms();

    if (!g_pr.have_z) { g_pr.have_z = true; g_pr.last_z = z; g_pr.last_t = now; }

    if (ground) {
        g_pr.ground_z = z;
        g_pr.have_ground_z = true;
        g_pr.vz = 0.0f;
        g_pr.last_z = z;
        g_pr.last_t = now;
        g_pr.tti = -1.0f;
        return;
    }

    // Z is written once per tick, so only recompute when it actually moved --
    // otherwise dt shrinks without new information and the velocity spikes.
    if (std::fabs(z - g_pr.last_z) >= 0.05f) {
        const double dt = (now - g_pr.last_t) / 1000.0;
        if (dt > 0.0005 && dt < 0.25) {
            const float v = static_cast<float>((z - g_pr.last_z) / dt);
            g_pr.vz = g_pr.vz * 0.4f + v * 0.6f;
        }
        g_pr.last_z = z;
        g_pr.last_t = now;
    }

    if (!g_pr.have_ground_z || g_pr.vz >= -kMinFallSpeed) { g_pr.tti = -1.0f; return; }

    const float dz = z - g_pr.ground_z;
    if (dz <= 0.0f) { g_pr.tti = 0.0f; return; }

    const float tti = (dz / -g_pr.vz) * 1000.0f;
    g_pr.tti = (tti > kMaxTtiMs) ? -1.0f : tti;
}

// ── injection ────────────────────────────────────────────────────────────
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

    bool   pressed = false;
    double press_at = 0.0;
    double next_press = 0.0;
    bool   armed = false;

    while (!g_stop.load()) {
        wait_ms(1.0);
        if (!g_mem.is_valid()) continue;

        const bool focused = cs2_focused();
        const bool space   = space_held();
        g_dbg_focus.store(focused);
        g_dbg_space.store(space);

        const bool driving = g_enabled.load() && focused && space;
        g_suppress.store(driving);

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

        // 1 ms sampling: prediction resolution is the lead budget, so losing
        // 2 ms to a 2 ms sample would eat a quarter of the lead.
        const float z = g_mem.read<float>(pawn + offsets::m_vOldOrigin + 8);
        if (now - last_sample >= 1.0) {
            last_sample = now;
            ground = sample_ground(g_mem, pawn);
        }

        if (ground) armed = false;
        predict_update(z, ground);

        if (pressed && (now - press_at) >= kHoldMs) {
            key_up();
            pressed = false;
        }

        bool start = false;
        if (ground) {
            // Fallback: we can see we are grounded and no jump happened, so
            // press. This is the path that stops a miss from ending the chain.
            if (next_press <= 0.0) next_press = now;
            if (now >= next_press) start = true;
        } else {
            next_press = 0.0;
            if (!armed && g_pr.tti >= 0.0f && g_pr.tti <=
                static_cast<float>(kLeadMs)) {
                start = true;
                armed = true;
            }
        }

        if (start && !pressed) {
            key_down();
            pressed = true;
            press_at = now;
            if (ground) next_press = now + kRetryMs;
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
    d.focused    = g_dbg_focus.load();
    d.space_held = g_dbg_space.load();
    d.hook_ok    = g_dbg_hook.load();
    d.on_ground  = g_dbg_ground.load();
    d.pressing   = g_dbg_pressing.load();
    return d;
}

void Bhop_SetEnabled(bool on) { g_enabled.store(on); }
bool Bhop_Enabled()           { return g_enabled.load(); }
