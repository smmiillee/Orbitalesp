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
extern ESP g_esp;
extern int g_local_team;
extern int g_screen_w;
extern int g_screen_h;

namespace {

// ===========================================================================
// HARDCODED BEHAVIOUR
// ===========================================================================
constexpr bool kTeamCheck = true;    // never shoot teammates
constexpr bool kVisCheck  = true;    // soft visibility gate
constexpr bool kFiring    = true;    // inject clicks

// Sustained-occlusion window before the vis check blocks. It only applies when
// the spotted mask reads ZERO, so it cannot stop wall shots -- a wall shot is
// the mask staying at 1 after line of sight breaks, which is radar persistence.
constexpr double kVisBlockAfterMs = 400.0;

// How many nonzero mask reads before the vis check is trusted. Until then it
// never blocks, so a wrong offset cannot refuse every shot.
constexpr int kVisTrustSamples = 1;

// ===========================================================================
// HIT TEST
// Wireframe mesh with a WORLD-SPACE radius, converted per target from the
// projected height, so the margin is proportional at every distance.
// ===========================================================================
constexpr float kMeshRadiusUnits = 4.5f;
constexpr float kHeadRadiusUnits = 3.2f;

// Extra links filling the gaps the bone chain leaves: clavicles, spine_0 and the
// forearms.
struct ExtraLink { int a, b; };
constexpr ExtraLink kExtraLinks[] = {
    { BONE_SPINE_0,    BONE_SPINE_1    },
    { BONE_PELVIS,     BONE_SPINE_0    },
    { BONE_CLAVICLE_L, BONE_L_SHOULDER },
    { BONE_CLAVICLE_R, BONE_R_SHOULDER },
    { BONE_CHEST,      BONE_CLAVICLE_L },
    { BONE_CHEST,      BONE_CLAVICLE_R },
    { BONE_L_HAND,     BONE_L_ELBOW    },
    { BONE_R_HAND,     BONE_R_ELBOW    },
};
constexpr int kExtraLinkCount =
    static_cast<int>(sizeof(kExtraLinks) / sizeof(kExtraLinks[0]));

// ---- per-weapon shot interval, ms: cycle times, so shots pace to the gun ----
struct WeaponTiming { int id; int interval_ms; };
constexpr WeaponTiming kTimings[] = {
    {  1, 267 },  {  2, 120 },  {  3, 150 },  {  4, 150 },
    {  7, 100 },  {  8,  90 },  {  9, 1460 }, { 10,  90 },
    { 11, 250 },  { 13,  90 },  { 14,  80 },  { 16,  90 },
    { 17,  70 },  { 19,  70 },  { 23,  80 },  { 24,  90 },
    { 25, 250 },  { 26,  70 },  { 27, 850 },  { 28,  70 },
    { 29, 850 },  { 30, 120 },  { 31, 400 },  { 32, 150 },
    { 33,  80 },  { 34,  70 },  { 35, 850 },  { 36, 150 },
    { 38, 250 },  { 39,  90 },  { 40, 1250 }, { 60, 100 },
    { 61, 150 },  { 63,  70 },  { 64, 350 },
};
constexpr int kDefaultIntervalMs = 120;
constexpr int kMinIntervalMs     = 60;

int weapon_interval(int id) {
    for (const WeaponTiming& w : kTimings)
        if (w.id == id) return w.interval_ms;
    return kDefaultIntervalMs;
}

std::atomic<bool> g_stop{false}, g_started{false};
std::atomic<bool> g_on{false}, g_firing{false};
std::atomic<int>  g_key{0};       // 0 == unbound == OFF
std::atomic<int>  g_delay{0};

std::thread g_thread;

double now_ms() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0).count();
}

// Sleep the bulk, spin only the last stretch. Used to throttle the loop, which
// otherwise ran as an unbounded yield() spin at TIME_CRITICAL priority -- each
// iteration calling project(), which takes the ESP mutex and does a
// ReadProcessMemory. That was the single biggest cost this process added to the
// game's frame time.
void wait_ms(double ms) {
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
    SendInput(1, &in, sizeof(INPUT));
}

void mouse_up() {
    INPUT in{};
    in.type = INPUT_MOUSE;
    in.mi.dwFlags = MOUSEEVENTF_LEFTUP;
    SendInput(1, &in, sizeof(INPUT));
}

// Radar-spotted mask. APPROXIMATE visibility -- radar state, not line of sight.
uint32_t spotted_mask(const Memory& mem, uintptr_t pawn) {
    return mem.read<uint32_t>(
        pawn + offsets::m_entitySpottedState + offsets::m_bSpottedByMask);
}

// 2D distance from a point to the line between two projected joints, clamped to
// the endpoints, so the whole limb is covered rather than just the joints.
float point_segment_dist(const Vec2& p, const Vec2& a, const Vec2& b) {
    const float vx = b.x - a.x, vy = b.y - a.y;
    const float wx = p.x - a.x, wy = p.y - a.y;

    const float len2 = vx * vx + vy * vy;
    if (len2 < 0.0001f) {
        const float dx = p.x - a.x, dy = p.y - a.y;
        return std::sqrt(dx * dx + dy * dy);
    }

    float t = (wx * vx + wy * vy) / len2;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    const float dx = p.x - (a.x + vx * t);
    const float dy = p.y - (a.y + vy * t);
    return std::sqrt(dx * dx + dy * dy);
}

// World-space radius to screen pixels at this target's distance. A player is
// about 72 world units tall, so pixels-per-unit follows from the projected
// height.
float screen_radius_for(const Vec2& head, const Vec2& feet, float world_units) {
    const float px_height = std::fabs(feet.y - head.y);
    if (px_height < 1.0f) return 1.0f;

    float r = world_units * (px_height / 72.0f);
    if (r < 1.0f)  r = 1.0f;
    if (r > 40.0f) r = 40.0f;
    return r;
}

void run_thread() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    bool   held = false;
    double next_shot = 0.0;

    int    weapon_id = 0;
    double weapon_at = -1e9;

    bool   vis_trusted = false;
    double vis_zero_since = 0.0;
    uintptr_t vis_zero_for = 0;

    double target_since = 0.0;   // when the current target was acquired

    while (!g_stop.load()) {
        const int k = g_key.load();
        const bool gate = g_on.load() && g_mem.is_valid() && cs2_focused() &&
                          k != 0 && ((GetAsyncKeyState(k) & 0x8000) != 0);

        // Idle: sleeping here makes a disarmed trigger essentially free.
        if (!gate) {
            if (held) { mouse_up(); held = false; }
            g_firing.store(false);
            target_since = 0.0;
            wait_ms(10.0);
            continue;
        }

        const int sw = g_screen_w, sh = g_screen_h;
        if (sw <= 0 || sh <= 0) { wait_ms(2.0); continue; }

        const double now = now_ms();

        if (now - weapon_at > 250.0) {
            weapon_at = now;
            weapon_id = ESP_LocalWeaponId(g_mem, g_mem.client_dll);
        }

        // Newest sample, no interpolation: firing on a smoothed position would
        // aim behind a moving target.
        const std::vector<PlayerESP> players =
            g_esp.project(g_mem, g_mem.client_dll, sw, sh, false);

        const Vec2 cross{ sw * 0.5f, sh * 0.5f };

        bool on_target = false;

        for (const PlayerESP& p : players) {
            if (p.team == 0) continue;
            if (kTeamCheck && g_local_team != 0 && p.team == g_local_team)
                continue;
            if (!p.has_bones) continue;

            const Vec2 head = p.bones[BONE_HEAD];
            const float radius =
                screen_radius_for(head, p.screen_feet, kMeshRadiusUnits);
            const float head_r =
                screen_radius_for(head, p.screen_feet, kHeadRadiusUnits);

            bool hit = false;

            for (int i = 0; i < kBoneLinkCount && !hit; ++i) {
                const int a = kBoneLinks[i].a, b = kBoneLinks[i].b;
                if (!p.bone_ok[a] || !p.bone_ok[b]) continue;
                if (point_segment_dist(cross, p.bones[a], p.bones[b]) <= radius)
                    hit = true;
            }

            for (int i = 0; i < kExtraLinkCount && !hit; ++i) {
                const int a = kExtraLinks[i].a, b = kExtraLinks[i].b;
                if (!p.bone_ok[a] || !p.bone_ok[b]) continue;
                if (point_segment_dist(cross, p.bones[a], p.bones[b]) <= radius)
                    hit = true;
            }

            if (!hit && p.bone_ok[BONE_HEAD]) {
                const float dx = head.x - cross.x, dy = head.y - cross.y;
                if (std::sqrt(dx * dx + dy * dy) <= head_r) hit = true;
            }

            if (!hit) continue;

            // ---- vis check: soft gate, blocks only after sustained zeros ----
            if (kVisCheck && p.pawn) {
                const uint32_t mask = spotted_mask(g_mem, p.pawn);
                if (mask != 0) {
                    if (kVisTrustSamples <= 1) vis_trusted = true;
                    vis_zero_for = 0;
                    vis_zero_since = 0.0;
                } else if (vis_trusted) {
                    if (vis_zero_for != p.pawn || vis_zero_since <= 0.0) {
                        vis_zero_for = p.pawn;
                        vis_zero_since = now;
                    } else if ((now - vis_zero_since) >= kVisBlockAfterMs) {
                        continue;   // sustained occlusion: skip this target
                    }
                }
            }

            on_target = true;
            break;
        }

        // ---- delay: reaction time before the FIRST shot only ----
        if (!on_target) {
            target_since = 0.0;
        } else if (target_since <= 0.0) {
            target_since = now;
        }

        const int delay = g_delay.load();
        const bool ready = (delay <= 0) ||
                           (target_since > 0.0 &&
                            (now - target_since) >= static_cast<double>(delay));

        // ---- fire, paced by the weapon's own cycle time ----
        const int interval = weapon_interval(weapon_id);
        const int clamped  = interval < kMinIntervalMs ? kMinIntervalMs : interval;

        if (kFiring && on_target && ready) {
            if (!held) {
                mouse_down();
                held = true;
                next_shot = now + clamped;
                g_firing.store(true);
            } else if (now >= next_shot) {
                // A shot registers on the released -> pressed transition, so a
                // fresh edge is required for each shot.
                mouse_up();
                mouse_down();
                next_shot = now + clamped;
            }
        } else if (held) {
            mouse_up();
            held = false;
            g_firing.store(false);
        }

        // Throttle while armed: 2 ms is 500 Hz, far above any weapon's cycle
        // time and well past what a triggerbot needs.
        wait_ms(2.0);
    }

    if (held) mouse_up();
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
    d.enabled = g_on.load();
    d.firing  = g_firing.load();
    return d;
}

void Aim_SetEnabled(bool on) { g_on.store(on); }
bool Aim_Enabled() { return g_on.load(); }
void Aim_SetKey(int vk) { g_key.store(vk); }
int  Aim_Key() { return g_key.load(); }

void Aim_SetDelay(int ms) {
    if (ms < 0) ms = 0;
    if (ms > 600) ms = 600;
    g_delay.store(ms);
}
int Aim_Delay() { return g_delay.load(); }
