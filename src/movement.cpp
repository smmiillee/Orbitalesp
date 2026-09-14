// --- src/movement.cpp ---
// Jumpbug. CTRL injection only. No memory writes.
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

// Crouch this many ms before the predicted landing. Wider than the bhop's lead
// because the crouch has to be DOWN at impact rather than produce an edge, and a
// longer hold covers more of the landing frames.
constexpr double kCrouchLeadMs  = 20.0;

// Keep CTRL down this long after touchdown, then release. The tail is what
// makes the fall-damage negation reliable: it does not depend on catching one
// exact frame.
constexpr double kCrouchTailMs  = 60.0;

// Below this the fall is too slow to be a real landing.
constexpr float kMinFallSpeed = 60.0f;

// Above this the fall has barely started; don't pre-crouch off a tiny drop.
constexpr float kMaxTtiMs = 500.0f;

std::atomic<bool> g_stop{false}, g_started{false};
std::atomic<bool> g_jumpbug{false};

std::atomic<bool>  g_dbg_arming{false}, g_dbg_ground{false};
std::atomic<bool>  g_dbg_crouch{false};
std::atomic<float> g_dbg_vz{0.0f}, g_dbg_tti{-1.0f};
std::atomic<int>   g_dbg_count{0};

std::thread g_thread;

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

void ctrl_down() {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = VK_CONTROL;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_CONTROL, MAPVK_VK_TO_VSC));
    in.ki.dwExtraInfo = 0;
    SendInput(1, &in, sizeof(INPUT));
}

void ctrl_up() {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = VK_CONTROL;
    in.ki.wScan = static_cast<WORD>(MapVirtualKeyW(VK_CONTROL, MAPVK_VK_TO_VSC));
    in.ki.dwFlags = KEYEVENTF_KEYUP;
    in.ki.dwExtraInfo = 0;
    SendInput(1, &in, sizeof(INPUT));
}

// Ground + vertical velocity, both from verified offsets.
struct Watch {
    bool   have_z = false;
    float  z = 0.0f;
    double last_t = 0.0;
    float  vz = 0.0f;
    float  ground_z = 0.0f;
    bool   have_ground_z = false;
    bool   last_ground = false;
    double z_changed = 0.0;
    bool   z_ground = true;
};
Watch g_w;

void update_watch(const Memory& mem, uintptr_t pawn) {
    const double now = now_ms();

    const float    z = mem.read<float>(pawn + offsets::m_vOldOrigin + 8);
    const uint32_t fl = mem.read<uint32_t>(pawn + offsets::m_fFlags);

    if (!g_w.have_z) {
        g_w.have_z = true; g_w.z = z; g_w.last_t = now; g_w.z_changed = now;
    }

    // Z stability gives a ground verdict independent of m_fFlags.
    if (std::fabs(z - g_w.z) >= 0.5f) { g_w.z = z; g_w.z_changed = now; }
    g_w.z_ground = (now - g_w.z_changed) > 22.0;

    const bool ground = g_w.z_ground || ((fl & 1u) != 0u);
    g_dbg_ground.store(ground);

    if (ground) {
        g_w.ground_z = z;
        g_w.have_ground_z = true;
        g_w.vz = 0.0f;
        g_w.last_ground = true;
        g_dbg_vz.store(0.0f);
        g_dbg_tti.store(-1.0f);
        return;
    }

    // Velocity from distinct Z samples only: Z is written once per tick, so
    // recomputing without new information makes the estimate spike.
    if (std::fabs(z - g_w.z) >= 0.05f) {
        const double dt = (now - g_w.last_t) / 1000.0;
        if (dt > 0.0005 && dt < 0.25) {
            const float v = static_cast<float>((z - g_w.z) / dt);
            g_w.vz = g_w.vz * 0.35f + v * 0.65f;
        }
        g_w.last_t = now;
    }
    g_w.z = z;
    g_dbg_vz.store(g_w.vz);

    if (!g_w.have_ground_z || g_w.vz >= -kMinFallSpeed) {
        g_dbg_tti.store(-1.0f);
        return;
    }

    const float dz = z - g_w.ground_z;
    if (dz <= 0.0f) { g_dbg_tti.store(0.0f); return; }

    const float tti = (dz / -g_w.vz) * 1000.0f;
    g_dbg_tti.store(tti > kMaxTtiMs ? -1.0f : tti);
}

void run_thread() {
    uintptr_t pawn = 0;
    double    pawn_at = 0.0;

    bool   crouching = false;
    bool   armed = false;          // we have committed to a crouch for this fall
    double release_at = 0.0;

    while (!g_stop.load()) {
        wait_ms(1.0);
        if (!g_mem.is_valid() || !g_jumpbug.load()) {
            if (crouching) { ctrl_up(); crouching = false; }
            armed = false;
            g_dbg_crouch.store(false);
            g_dbg_arming.store(false);
            continue;
        }

        const bool focused = cs2_focused();
        const double now = now_ms();
        if (!pawn || now - pawn_at > 500.0) {
            pawn = g_mem.read<uintptr_t>(
                g_mem.client_dll + offsets::dwLocalPlayerPawn);
            pawn_at = now;
        }
        if (!pawn || !focused) {
            if (crouching) { ctrl_up(); crouching = false; }
            continue;
        }

        update_watch(g_mem, pawn);

        const bool  ground = g_dbg_ground.load();
        const float tti    = g_dbg_tti.load();
        const float vz     = g_dbg_vz.load();

        g_dbg_arming.store(true);

        // ── arm the crouch before the landing ────────────────────────────
        if (!ground && !armed && vz < -kMinFallSpeed &&
            tti >= 0.0f && tti <= kCrouchLeadMs) {
            ctrl_down();
            crouching = true;
            armed = true;
            release_at = now + kCrouchLeadMs + kCrouchTailMs;
            g_dbg_count.fetch_add(1);
        }

        // ── release: the tail runs from arm time, which covers the landing ─
        // Held through touchdown on purpose: the damage-negation window is
        // wide, so holding across it is what makes that part reliable.
        if (crouching && now >= release_at) {
            ctrl_up();
            crouching = false;
            armed = false;      // re-arms only after ground is re-established
        }

        // Once genuinely grounded again, clear the arm so the next fall can fire.
        if (ground && crouching && now >= release_at) {
            ctrl_up();
            crouching = false;
            armed = false;
        }

        g_dbg_crouch.store(crouching);
    }

    if (crouching) ctrl_up();
}

} // namespace

void Movement_Init() {
    if (!g_mem.is_valid()) return;
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;

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
    d.arming    = g_dbg_arming.load();
    d.on_ground = g_dbg_ground.load();
    d.crouching = g_dbg_crouch.load();
    d.vz        = g_dbg_vz.load();
    d.tti       = g_dbg_tti.load();
    d.jumpbugs  = g_dbg_count.load();
    return d;
}

void Movement_SetJumpbug(bool on) { g_jumpbug.store(on); }
bool Movement_Jumpbug()           { return g_jumpbug.load(); }
