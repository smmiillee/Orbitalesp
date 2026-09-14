// --- src/aim.cpp ---
// Triggerbot. Detection is read-only; firing injects a mouse click.
#include "aim.h"
#include "esp.h"
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

// The ESP instance lives in main.cpp.
extern ESP g_esp;

namespace {

// Bone ids worth shooting at, in order of preference.
struct Hit { int bone; const char* name; };
constexpr Hit kHits[] = {
    { BONE_HEAD,  "head"  },
    { BONE_NECK,  "neck"  },
    { BONE_CHEST, "chest" },
    { BONE_PELVIS,"pelvis"},
    { BONE_SPINE_1,"spine"},
};

std::atomic<bool> g_stop{false}, g_started{false};
std::atomic<bool> g_enabled{false}, g_fire{true};
std::atomic<int>  g_key{0};

std::atomic<bool>  g_dbg_firing{false}, g_dbg_on{false};
std::atomic<float> g_dbg_dist{0.0f};
std::atomic<const char*> g_dbg_bone{"-"};

std::thread g_thread;

// A click must be held long enough to be sampled by at least one frame, for the
// same reason the bhop key does: a zero-width press is frequently never seen.
constexpr double kClickHoldMs = 12.0;
// Minimum gap between shots, so one engagement is one bullet.
constexpr double kCooldownMs  = 120.0;

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

void mouse_down() {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_LEFTDOWN;
    in.mi.dwExtraInfo = 0;
    SendInput(1, &in, sizeof(INPUT));
}

void mouse_up() {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    in.mi.dwExtraInfo = 0;
    SendInput(1, &in, sizeof(INPUT));
}

void run_thread() {
    bool   down = false;
    double down_at = 0.0;
    double next_shot = 0.0;

    while (!g_stop.load()) {
        wait_ms(2.0);

        if (!g_enabled.load() || !g_mem.is_valid() || !cs2_focused()) {
            if (down) { mouse_up(); down = false; }
            g_dbg_firing.store(false);
            g_dbg_on.store(false);
            continue;
        }

        // Aim key gate. 0 means always active.
        const int key = g_key.load();
        if (key && !(GetAsyncKeyState(key) & 0x8000)) {
            if (down) { mouse_up(); down = false; }
            g_dbg_firing.store(false);
            g_dbg_on.store(false);
            continue;
        }

        if (down && (now_ms() - down_at) >= kClickHoldMs) {
            mouse_up();
            down = false;
            g_dbg_firing.store(false);
        }

        const double now = now_ms();
        if (now < next_shot) continue;

        // Newest sample, not the interpolated one: firing on a 35 ms-old
        // position would mean aiming behind a moving target.
        const int sw = GetSystemMetrics(SM_CXSCREEN);
        const int sh = GetSystemMetrics(SM_CYSCREEN);
        const std::vector<PlayerESP> players =
            g_esp.project(g_mem, g_mem.client_dll, sw, sh, false);

        const float cx = sw * 0.5f;
        const float cy = sh * 0.5f;

        // Radius scales with screen height so it behaves the same at any
        // resolution. ~1.4% of height is a tight, head-sized window.
        const float radius = sh * 0.014f;

        bool  found = false;
        float best = 1e9f;
        const char* best_name = "-";

        for (const PlayerESP& p : players) {
            if (p.team == 0) continue;
            if (p.team == g_esp.local_team_now) continue;   // never teammates

            for (const Hit& h : kHits) {
                if (!p.bone_ok[h.bone]) continue;
                const float dx = p.bones[h.bone].x - cx;
                const float dy = p.bones[h.bone].y - cy;
                const float d  = std::sqrt(dx * dx + dy * dy);
                if (d > radius) continue;
                if (d < best) { best = d; best_name = h.name; found = true; }
            }
        }

        g_dbg_on.store(found);
        g_dbg_dist.store(found ? best : -1.0f);
        g_dbg_bone.store(found ? best_name : "-");

        if (found && g_fire.load()) {
            mouse_down();
            down = true;
            down_at = now;
            next_shot = now + kCooldownMs;
            g_dbg_firing.store(true);
        }
    }

    if (down) mouse_up();
}

} // namespace

void Aim_Init() {
    if (!g_mem.is_valid()) return;
    bool expected = false;
    if (!g_started.compare_exchange_strong(expected, true)) return;

    timeBeginPeriod(1);
    g_stop.store(false);
    g_thread = std::thread(run_thread);
}

void Aim_Shutdown() {
    g_stop.store(true);
    if (g_thread.joinable()) g_thread.join();
    timeEndPeriod(1);
    g_started.store(false);
}

AimDebug Aim_GetDebug() {
    AimDebug d;
    d.enabled     = g_enabled.load();
    d.firing      = g_dbg_firing.load();
    d.on_target   = g_dbg_on.load();
    d.target_dist = g_dbg_dist.load();
    d.target_bone = g_dbg_bone.load();
    return d;
}

void Aim_SetEnabled(bool on) { g_enabled.store(on); }
bool Aim_Enabled()           { return g_enabled.load(); }

void Aim_SetFire(bool on) { g_fire.store(on); }
bool Aim_Fire()           { return g_fire.load(); }

void Aim_SetKey(int vk) { g_key.store(vk); }
int  Aim_Key()          { return g_key.load(); }
