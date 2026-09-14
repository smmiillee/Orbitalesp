// --- src/aim.cpp ---
// Triggerbot. Detection is read-only; firing injects a click.
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
extern ESP g_esp;
extern int g_local_team;
// The overlay's viewport. GetSystemMetrics would return the MONITOR size, which
// differs from the game client area on a scaled display, putting the crosshair
// centre in the wrong place.
extern int g_screen_w;
extern int g_screen_h;

namespace {

struct Hit { int bone; const char* name; };
constexpr Hit kHits[] = {
    {BONE_HEAD,"head"},{BONE_NECK,"neck"},{BONE_CHEST,"chest"},
    {BONE_PELVIS,"pelvis"},{BONE_SPINE_1,"spine"},
};

std::atomic<bool>  g_stop{false}, g_started{false};
std::atomic<bool>  g_on{false}, g_fire{true};
std::atomic<int>   g_key{0};
std::atomic<float> g_radius{1.4f};
std::atomic<int>   g_delay{0};

std::atomic<bool>  g_dbg_firing{false}, g_dbg_on{false};
std::atomic<float> g_dbg_dist{-1.0f};
std::atomic<const char*> g_dbg_bone{"-"};
std::atomic<int>   g_dbg_held{0};

std::thread g_thread;

constexpr double kClickMs = 12.0;
constexpr double kCoolMs  = 120.0;

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

void mouse(bool down) {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = down ? MOUSEEVENTF_LEFTDOWN : MOUSEEVENTF_LEFTUP;
    SendInput(1, &in, sizeof(INPUT));
}

void run_thread() {
    bool   down = false;
    double down_at = 0.0;
    double next_shot = 0.0;
    double target_since = 0.0;   // when the current target was acquired

    while (!g_stop.load()) {
        wait_ms(2.0);

        const int k = g_key.load();
        const bool gate = g_on.load() && g_mem.is_valid() && cs2_focused() &&
                          (k == 0 || (GetAsyncKeyState(k) & 0x8000));

        if (!gate) {
            if (down) { mouse(false); down = false; }
            g_dbg_firing.store(false);
            g_dbg_on.store(false);
            g_dbg_held.store(0);
            target_since = 0.0;
            continue;
        }

        if (down && now_ms() - down_at >= kClickMs) {
            mouse(false);
            down = false;
            g_dbg_firing.store(false);
        }

        const double now = now_ms();
        if (now < next_shot) continue;

        const int sw = g_screen_w, sh = g_screen_h;
        if (sw <= 0 || sh <= 0) continue;

        const std::vector<PlayerESP> players =
            g_esp.project(g_mem, g_mem.client_dll, sw, sh, false);

        const float cx = sw * 0.5f, cy = sh * 0.5f;
        const float radius = sh * (g_radius.load() / 100.0f);

        bool found = false;
        float best = 1e9f;
        const char* best_name = "-";

        for (const PlayerESP& p : players) {
            if (p.team == 0) continue;
            if (g_local_team != 0 && p.team == g_local_team) continue;
            for (const Hit& h : kHits) {
                if (!p.bone_ok[h.bone]) continue;
                const float dx = p.bones[h.bone].x - cx;
                const float dy = p.bones[h.bone].y - cy;
                const float d = std::sqrt(dx * dx + dy * dy);
                if (d > radius) continue;
                if (d < best) { best = d; best_name = h.name; found = true; }
            }
        }

        g_dbg_on.store(found);
        g_dbg_dist.store(found ? best : -1.0f);
        g_dbg_bone.store(found ? best_name : "-");

        // ---- acquisition delay ----
        // The target has to STAY acquired for `delay` ms before we shoot, which
        // is what a triggerbot delay means: it damps instant reactions and
        // makes the timing look less mechanical.
        if (!found) {
            target_since = 0.0;
            g_dbg_held.store(0);
            continue;
        }

        if (target_since <= 0.0) target_since = now;
        const int held = (int)(now - target_since);
        g_dbg_held.store(held);

        if (held < g_delay.load()) continue;

        if (g_fire.load()) {
            mouse(true);
            down = true;
            down_at = now;
            next_shot = now + kCoolMs;
            target_since = 0.0;
            g_dbg_firing.store(true);
        }
    }
    if (down) mouse(false);
}

} // namespace

void Aim_Init() {
    if (!g_mem.is_valid()) return;
    bool e = false;
    if (!g_started.compare_exchange_strong(e, true)) return;
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
    d.enabled     = g_on.load();
    d.firing      = g_dbg_firing.load();
    d.on_target   = g_dbg_on.load();
    d.target_dist = g_dbg_dist.load();
    d.target_bone = g_dbg_bone.load();
    d.delay_ms    = g_delay.load();
    d.held_ms     = g_dbg_held.load();
    return d;
}

void Aim_SetEnabled(bool on) { g_on.store(on); }
bool Aim_Enabled() { return g_on.load(); }
void Aim_SetFire(bool on) { g_fire.store(on); }
bool Aim_Fire() { return g_fire.load(); }
void Aim_SetKey(int vk) { g_key.store(vk); }
int  Aim_Key() { return g_key.load(); }

void Aim_SetRadius(float pct) {
    if (pct < 0.2f) pct = 0.2f;
    if (pct > 10.0f) pct = 10.0f;
    g_radius.store(pct);
}
float Aim_Radius() { return g_radius.load(); }

void Aim_SetDelay(int ms) {
    if (ms < 0) ms = 0;
    if (ms > 200) ms = 200;
    g_delay.store(ms);
}
int Aim_Delay() { return g_delay.load(); }
