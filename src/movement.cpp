// --- src/movement.cpp ---
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

constexpr double kLeadMs = 25.0;
constexpr double kTailMs = 70.0;
constexpr float  kMinFall = 40.0f;

std::atomic<bool> g_stop{false}, g_started{false};
std::atomic<bool> g_on{false};
std::atomic<int>  g_key{0};

std::atomic<bool>  g_dbg_ground{false}, g_dbg_crouch{false}, g_dbg_armed{false};
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
    while (std::chrono::steady_clock::now() < dl) {
        const auto left = dl - std::chrono::steady_clock::now();
        if (left > std::chrono::milliseconds(2))
            std::this_thread::sleep_for(left - std::chrono::milliseconds(1));
        else
            std::this_thread::yield();
    }
}

bool cs2_focused() {
    return g_cs2_hwnd && GetForegroundWindow() == g_cs2_hwnd;
}

void ctrl(bool down) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = VK_CONTROL;
    in.ki.wScan = (WORD)MapVirtualKeyW(VK_CONTROL, MAPVK_VK_TO_VSC);
    if (!down) in.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(INPUT));
}

// ── state ────────────────────────────────────────────────────────────────
// *** WHY tti WAS ALWAYS -1 ***
// The previous version only updated last_t when Z moved, but compared the
// CURRENT z against a g_w.z it had already overwritten -- so after the first
// sample the delta was always ~0 and the velocity never left zero. vz stayed
// above the fall threshold, so tti was never computed.
//
// This keeps explicit previous-sample state and updates it in exactly one
// place, so the finite difference is always between two real samples.
struct State {
    bool   has_prev = false;
    float  prev_z = 0.0f;
    double prev_t = 0.0;

    bool   have_ground_z = false;
    float  ground_z = 0.0f;

    float  vz = 0.0f;
    double still_since = 0.0;

    bool   on_ground = false;
    float  tti = -1.0f;
};
State g_s;

void update(const Memory& mem, uintptr_t pawn) {
    const double now = now_ms();

    const float z = mem.read<float>(pawn + offsets::m_vOldOrigin + 8);
    const bool  flag_ground =
        (mem.read<uint32_t>(pawn + offsets::m_fFlags) & 1u) != 0u;

    if (!g_s.has_prev) {
        g_s.has_prev = true; g_s.prev_z = z; g_s.prev_t = now;
        g_s.still_since = now;
        return;
    }

    const float dz = z - g_s.prev_z;
    const double dt = (now - g_s.prev_t) / 1000.0;

    // Z does not move while standing, so a still Z for ~22 ms means grounded.
    if (std::fabs(dz) >= 0.5f) g_s.still_since = now;
    const bool z_ground = (now - g_s.still_since) > 22.0;

    g_s.on_ground = z_ground || flag_ground;

    if (!g_s.on_ground && dt > 0.0005 && dt < 0.25 &&
        std::fabs(dz) >= 0.05f) {
        const float v = (float)(dz / dt);
        g_s.vz = g_s.vz * 0.4f + v * 0.6f;
    }

    g_s.prev_z = z;
    g_s.prev_t = now;

    if (g_s.on_ground) {
        g_s.ground_z = z;
        g_s.have_ground_z = true;
        g_s.vz = 0.0f;
        g_s.tti = -1.0f;
    } else if (g_s.have_ground_z && g_s.vz < -kMinFall) {
        const float gap = z - g_s.ground_z;
        g_s.tti = (gap <= 0.0f) ? 0.0f : (gap / -g_s.vz) * 1000.0f;
        if (g_s.tti > 600.0f) g_s.tti = -1.0f;
    } else {
        g_s.tti = -1.0f;
    }

    g_dbg_ground.store(g_s.on_ground);
    g_dbg_vz.store(g_s.vz);
    g_dbg_tti.store(g_s.tti);
}

void run_thread() {
    uintptr_t pawn = 0;
    double pawn_at = 0.0;

    bool crouch_down = false;
    bool armed = false;
    double release_at = 0.0;

    while (!g_stop.load()) {
        wait_ms(1.0);

        const int key = g_key.load();
        const bool gate = g_on.load() && cs2_focused() &&
                          (key == 0 || (GetAsyncKeyState(key) & 0x8000));

        if (!g_mem.is_valid() || !gate) {
            if (crouch_down) { ctrl(false); crouch_down = false; }
            armed = false;
            g_dbg_crouch.store(false);
            g_dbg_armed.store(false);
            continue;
        }

        const double now = now_ms();
        if (!pawn || now - pawn_at > 500.0) {
            pawn = g_mem.read<uintptr_t>(
                g_mem.client_dll + offsets::dwLocalPlayerPawn);
            pawn_at = now;
        }
        if (!pawn) continue;

        update(g_mem, pawn);
        g_dbg_armed.store(armed);

        if (g_s.on_ground && crouch_down && now >= release_at) {
            ctrl(false); crouch_down = false; armed = false;
        }

        if (!g_s.on_ground && !armed && g_s.tti >= 0.0f &&
            g_s.tti <= kLeadMs) {
            ctrl(true);
            crouch_down = true;
            armed = true;
            release_at = now + kLeadMs + kTailMs;
            g_dbg_n.fetch_add(1);
        }

        g_dbg_crouch.store(crouch_down);
    }

    if (crouch_down) ctrl(false);
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
    d.enabled = g_on.load();
    d.on_ground = g_dbg_ground.load();
    d.crouching = g_dbg_crouch.load();
    d.armed = g_dbg_armed.load();
    d.vz = g_dbg_vz.load();
    d.tti = g_dbg_tti.load();
    d.jumpbugs = g_dbg_n.load();
    return d;
}

void Movement_SetJumpbug(bool on) { g_on.store(on); }
bool Movement_Jumpbug() { return g_on.load(); }
void Movement_SetKey(int vk) { g_key.store(vk); }
int  Movement_Key() { return g_key.load(); }
