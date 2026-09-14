// --- src/movement.cpp ---
// Jumpbug. CTRL injection only, no memory writes.
//
// Uses the SAME prediction as the bhop: vertical velocity from distinct Z
// samples, and time-to-impact. Two timings are added on top:
//
//   crouch   - press CTRL when tti drops below kCrouchLeadMs
//   uncrouch - release CTRL when tti drops below kUncrouchLeadMs
//
// The uncrouch just before touchdown is the part that produces the bug. Both
// are constants at the top of this file, so they are easy to move.
//
// Independent of bhop: the Bhop toggle does not need to be on.
#include "movement.h"
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

// ---- timing, same style as bhop.cpp ----
constexpr double kCrouchLeadMs   = 150.0;  // crouch this early in the fall
constexpr double kUncrouchLeadMs = 24.0;   // uncrouch this close to touchdown
constexpr float  kMinFallSpeed   = 40.0f;  // units/sec
constexpr float  kMaxTtiMs       = 900.0f;

std::atomic<bool> g_stop{false}, g_started{false};
std::atomic<bool> g_on{false};
std::atomic<int>  g_key{0};

std::atomic<bool>  g_dbg_ground{false}, g_dbg_crouch{false};
std::atomic<float> g_dbg_vz{0.0f}, g_dbg_tti{-1.0f};
std::atomic<int>   g_dbg_n{0};

std::thread g_thread;

double now_ms() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0).count();
}

void wait_ms(double ms) {
    if (ms <= 0.0) return;
    const auto dl = std::chrono::steady_clock::now() +
        std::chrono::microseconds((long long)(ms * 1000.0));
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= dl) return;
        const auto left = dl - now;
        if (left > std::chrono::milliseconds(2))
            std::this_thread::sleep_for(left - std::chrono::milliseconds(1));
        else
            std::this_thread::yield();
    }
}

bool cs2_focused() {
    return g_cs2_hwnd && GetForegroundWindow() == g_cs2_hwnd;
}

// Separate down/up calls so the key is genuinely held, not a zero-width pulse.
void ctrl_down() {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = VK_CONTROL;
    in.ki.wScan = (WORD)MapVirtualKeyW(VK_CONTROL, MAPVK_VK_TO_VSC);
    SendInput(1, &in, sizeof(INPUT));
}

void ctrl_up() {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = VK_CONTROL;
    in.ki.wScan = (WORD)MapVirtualKeyW(VK_CONTROL, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(INPUT));
}

// ---- prediction, same structure as bhop.cpp ----
struct Watch {
    bool   has_prev = false;
    float  prev_z = 0.0f;
    double prev_t = 0.0;
    double still_since = 0.0;
    float  vz = 0.0f;
    float  ground_z = 0.0f;
    bool   have_ground_z = false;
    bool   on_ground = false;
    float  tti = -1.0f;
};
Watch g_w;

void update_watch(const Memory& mem, uintptr_t pawn) {
    const double now = now_ms();

    const float z = mem.read<float>(pawn + offsets::m_vOldOrigin + 8);
    const bool flag_ground =
        (mem.read<uint32_t>(pawn + offsets::m_fFlags) & 1u) != 0u;

    if (!g_w.has_prev) {
        g_w.has_prev = true;
        g_w.prev_z = z;
        g_w.prev_t = now;
        g_w.still_since = now;
        return;
    }

    const float dz = z - g_w.prev_z;
    const double dt = (now - g_w.prev_t) / 1000.0;

    if (std::fabs(dz) >= 0.5f) g_w.still_since = now;
    const bool z_ground = (now - g_w.still_since) > 22.0;
    g_w.on_ground = z_ground || flag_ground;

    if (!g_w.on_ground && dt > 0.0005 && dt < 0.25 &&
        std::fabs(dz) >= 0.05f) {
        const float v = (float)(dz / dt);
        g_w.vz = g_w.vz * 0.4f + v * 0.6f;
    }

    g_w.prev_z = z;
    g_w.prev_t = now;

    if (g_w.on_ground) {
        g_w.ground_z = z;
        g_w.have_ground_z = true;
        g_w.vz = 0.0f;
        g_w.tti = -1.0f;
    } else if (g_w.have_ground_z && g_w.vz < -kMinFallSpeed) {
        const float gap = z - g_w.ground_z;
        const float t = (gap <= 0.0f) ? 0.0f : (gap / -g_w.vz) * 1000.0f;
        g_w.tti = (t > kMaxTtiMs) ? -1.0f : t;
    } else {
        g_w.tti = -1.0f;
    }

    g_dbg_ground.store(g_w.on_ground);
    g_dbg_vz.store(g_w.vz);
    g_dbg_tti.store(g_w.tti);
}

void run_thread() {
    uintptr_t pawn = 0;
    double pawn_at = 0.0;

    bool crouching = false;
    bool done = false;    // one crouch/uncrouch per airtime

    while (!g_stop.load()) {
        wait_ms(1.0);

        const int key = g_key.load();
        const bool gate = g_on.load() && cs2_focused() &&
                          (key == 0 || (GetAsyncKeyState(key) & 0x8000));

        if (!g_mem.is_valid() || !gate) {
            if (crouching) { ctrl_up(); crouching = false; }
            done = false;
            g_dbg_crouch.store(false);
            continue;
        }

        const double now = now_ms();
        if (!pawn || now - pawn_at > 500.0) {
            pawn = g_mem.read<uintptr_t>(
                g_mem.client_dll + offsets::dwLocalPlayerPawn);
            pawn_at = now;
        }
        if (!pawn) continue;

        update_watch(g_mem, pawn);

        // Re-arm once safely on the ground.
        if (g_w.on_ground) {
            if (crouching) { ctrl_up(); crouching = false; }
            done = false;
            g_dbg_crouch.store(false);
            continue;
        }

        const float tti = g_w.tti;

        // ---- crouch early in the fall ----
        if (!done && !crouching && tti >= 0.0f && tti <= kCrouchLeadMs) {
            ctrl_down();
            crouching = true;
            g_dbg_n.fetch_add(1);
        }

        // ---- uncrouch just before touchdown: this is the bug ----
        if (crouching && tti >= 0.0f && tti <= kUncrouchLeadMs) {
            ctrl_up();
            crouching = false;
            done = true;
        }

        g_dbg_crouch.store(crouching);
    }

    if (crouching) ctrl_up();
}

} // namespace

void Movement_Init() {
    if (!g_mem.is_valid()) return;
    bool e = false;
    if (!g_started.compare_exchange_strong(e, true)) return;
    timeBeginPeriod(1);
    g_stop.store(false);
    g_thread = std::thread(run_thread);
}

void Movement_Shutdown() {
    g_stop.store(true);
    if (g_thread.joinable()) g_thread.join();
    timeEndPeriod(1);
    g_started.store(false);
}

MovementDebug Movement_GetDebug() {
    MovementDebug d;
    d.enabled   = g_on.load();
    d.on_ground = g_dbg_ground.load();
    d.crouching = g_dbg_crouch.load();
    d.armed     = g_dbg_crouch.load();
    d.vz        = g_dbg_vz.load();
    d.tti       = g_dbg_tti.load();
    d.jumpbugs  = g_dbg_n.load();
    return d;
}

void Movement_SetJumpbug(bool on) { g_on.store(on); }
bool Movement_Jumpbug() { return g_on.load(); }
void Movement_SetKey(int vk) { g_key.store(vk); }
int  Movement_Key() { return g_key.load(); }
