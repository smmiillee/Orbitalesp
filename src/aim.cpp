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
extern int g_screen_w;
extern int g_screen_h;

namespace {

// ---- HARDCODED BONE RADII ----
// Authored against a 1080p reference and scaled by actual height, so behaviour
// is identical at any resolution. No user adjustment, as requested.
struct Hit { int bone; const char* name; float radius_px_at_1080; };
constexpr Hit kHits[] = {
    { BONE_HEAD,    "head",   14.0f },
    { BONE_NECK,    "neck",   12.0f },
    { BONE_CHEST,   "chest",  16.0f },
    { BONE_SPINE_1, "spine",  16.0f },
    { BONE_PELVIS,  "pelvis", 15.0f },
};
constexpr float kReferenceHeight = 1080.0f;

std::atomic<bool>  g_stop{false}, g_started{false};
std::atomic<bool>  g_on{false}, g_fire{true};
std::atomic<bool>  g_teamcheck{true}, g_vischeck{false};
std::atomic<int>   g_key{0};        // 0 == unbound == OFF
std::atomic<int>   g_delay{0};

std::atomic<bool>  g_dbg_firing{false}, g_dbg_on{false};
std::atomic<bool>  g_dbg_vis{false},    g_dbg_visable{false};
std::atomic<float> g_dbg_dist{-1.0f};
std::atomic<const char*> g_dbg_bone{"-"};
std::atomic<const char*> g_dbg_blocked{"-"};
std::atomic<int>   g_dbg_held{0};
std::atomic<int>   g_vis_samples{0}, g_vis_hits{0};

std::thread g_thread;

// The click must be held long enough to be sampled by a frame.
constexpr double kClickMs = 12.0;

// *** THIS WAS THE "DELAYED EVEN AT 0 ms" CAUSE ***
// 120 ms of cooldown after every shot meant the effective cadence on a held
// target was 120 ms regardless of the delay slider. The cooldown is now small --
// just enough that one engagement is one bullet -- and the DELAY is the only
// meaningful timing gate.
constexpr double kCoolMs = 16.0;

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

// Radar-spotted mask. APPROXIMATE visibility, see aim.h -- this is radar state,
// not line of sight, so it cannot know what YOU can see.
uint32_t spotted_mask(const Memory& mem, uintptr_t pawn) {
    return mem.read<uint32_t>(
        pawn + offsets::m_entitySpottedState + offsets::m_bSpottedByMask);
}

void run_thread() {
    bool   down = false;
    double down_at = 0.0;
    double next_shot = 0.0;
    double target_since = 0.0;

    // The vis check proves itself by ever reading a nonzero mask. Until it has,
    // it is untrustworthy and does not block firing.
    bool vis_trusted = false;

    while (!g_stop.load()) {
        wait_ms(2.0);

        const int k = g_key.load();

        // UNBOUND MEANS OFF. key == 0 is not "always on".
        const bool gate = g_on.load() && g_mem.is_valid() && cs2_focused() &&
                          k != 0 && ((GetAsyncKeyState(k) & 0x8000) != 0);

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
        if (now < next_shot) {
            // Do NOT clear target_since here. Clearing it meant the delay could
            // never accumulate while a target was continuously held, so the
            // delay appeared ignored. It keeps counting now, and the cooldown is
            // short enough (16 ms) not to mask it.
            if (target_since > 0.0)
                g_dbg_held.store((int)(now - target_since));
            continue;
        }

        const int sw = g_screen_w, sh = g_screen_h;
        if (sw <= 0 || sh <= 0) continue;

        const std::vector<PlayerESP> players =
            g_esp.project(g_mem, g_mem.client_dll, sw, sh, false);

        const float cx = sw * 0.5f, cy = sh * 0.5f;
        const float scale = (float)sh / kReferenceHeight;

        const uintptr_t local_pawn =
            g_mem.read<uintptr_t>(g_mem.client_dll + offsets::dwLocalPlayerPawn);

        bool  found = false, blocked = false, has_vis = false;
        float best = 1e9f;
        const char* best_name = "-";
        const char* why = "-";

        for (const PlayerESP& p : players) {
            if (p.team == 0) continue;

            if (g_teamcheck.load() && g_local_team != 0 &&
                p.team == g_local_team)
                continue;

            // Which hardcoded bone window is the crosshair inside?
            const Hit* in_bone = nullptr;
            for (const Hit& h : kHits) {
                if (!p.bone_ok[h.bone]) continue;
                const float dx = p.bones[h.bone].x - cx;
                const float dy = p.bones[h.bone].y - cy;
                const float d = std::sqrt(dx * dx + dy * dy);
                if (d > h.radius_px_at_1080 * scale) continue;
                if (!in_bone || d < best) {
                    in_bone = &h;
                    best = d;
                    best_name = h.name;
                }
            }
            if (!in_bone) continue;

            found = true;

            // ---- approximate visibility ----
            if (g_vischeck.load() && p.pawn) {
                const uint32_t mask = spotted_mask(g_mem, p.pawn);
                ++g_vis_samples;
                if (mask != 0) {
                    ++g_vis_hits;
                    vis_trusted = true;
                }

                if (vis_trusted) {
                    if (mask == 0) {
                        blocked = true;
                        why = "vis";
                        continue;
                    }
                    has_vis = true;
                }
            } else {
                has_vis = true;
            }
        }

        g_dbg_on.store(found);
        g_dbg_vis.store(has_vis);
        g_dbg_visable.store(vis_trusted);
        g_dbg_dist.store(found ? best : -1.0f);
        g_dbg_bone.store(found ? best_name : "-");
        g_dbg_blocked.store(blocked ? why : "-");

        if (blocked || !found) {
            target_since = 0.0;
            g_dbg_held.store(0);
            continue;
        }

        if (target_since <= 0.0) target_since = now;
        const int held = (int)(now - target_since);
        g_dbg_held.store(held);

        // With delay 0 this fires on the very next iteration, so 0 ms is
        // genuinely instant rather than gated by anything else.
        if (held < g_delay.load()) continue;

        if (g_fire.load() && local_pawn) {
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
    d.has_vis     = g_dbg_vis.load();
    d.vis_usable  = g_dbg_visable.load();
    d.vis_samples = g_vis_samples.load();
    d.vis_hits    = g_vis_hits.load();
    d.blocked_by  = g_dbg_blocked.load();
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

void Aim_SetTeamCheck(bool on) { g_teamcheck.store(on); }
bool Aim_TeamCheck() { return g_teamcheck.load(); }

void Aim_SetVisCheck(bool on) { g_vischeck.store(on); }
bool Aim_VisCheck() { return g_vischeck.load(); }

void Aim_SetDelay(int ms) {
    if (ms < 0) ms = 0;
    if (ms > 600) ms = 600;
    g_delay.store(ms);
}
int Aim_Delay() { return g_delay.load(); }
