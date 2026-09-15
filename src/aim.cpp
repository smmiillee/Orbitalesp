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
constexpr bool kTeamCheck = true;
constexpr bool kFiring    = true;

// ===========================================================================
// VIS CHECK -- READ THIS BEFORE EXPECTING IT TO BE EXACT
//
// m_bSpotted / m_bSpottedByMask is RADAR state. It answers "has anyone on my
// team spotted this enemy recently", NOT "can I see them". Two consequences:
//
//   * It PERSISTS. An enemy seen by a teammate then moving behind a wall still
//     reads spotted, so a hard gate shoots that wall.
//   * It LAGS for you. An enemy YOU can see but no teammate has spotted reads
//     zero, so a hard gate refuses to fire -- which is exactly the "holding a
//     tight angle and it will not shoot" case.
//
// So it cannot be made exact externally. What we do instead is use it as a SOFT
// gate: only block once the mask has read zero CONTINUOUSLY for a while. A brief
// appearance is never blocked, and sustained occlusion does get blocked.
//
// Set kVisCheck to false to disable it entirely; the soft window is the only
// tuning knob and it is deliberately generous.
// ===========================================================================
constexpr bool   kVisCheck = true;
constexpr double kVisBlockAfterMs = 400.0;  // sustained occlusion before blocking

// Once the offset has read nonzero at least once we consider it usable. Until
// then it never blocks, so a wrong offset cannot refuse every shot. This is what
// makes the first rounds permissive rather than broken.
constexpr int kVisTrustSamples = 1;

// ===========================================================================
// HIT TEST: WIREFRAME MESH AT A WORLD-SPACE RADIUS
//
// The previous version used a radius fixed in SCREEN PIXELS, which is wrong: a
// distant player's limbs project tiny, so a fixed pixel radius over-covers them
// and fires beside the body. Near players get under-covered.
//
// The radius is now in WORLD units and converted to pixels per hitbox using the
// projection scale, so the margin is proportional at every distance.
// ===========================================================================
constexpr float kMeshRadiusUnits = 4.5f;   // world-space tube radius
constexpr float kHeadRadiusUnits = 3.2f;   // head sphere is tighter

// Head-only bonus: the head sphere is tested separately because it is not a
// link, and it is the most valuable hitbox.
constexpr bool kPreferHead = true;

// ===========================================================================
// EXTRA HITBOX POINTS
//
// The bone chain alone leaves gaps: the clavicles, spine_0 and the hands/feet
// are sparse. These links fill in the torso shoulders and the limbs so the mesh
// covers the whole playermodel rather than just the main joints.
// ===========================================================================
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

// ---- per-weapon shot interval, ms ----
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
std::atomic<bool> g_on{false};
std::atomic<int>  g_key{0};

std::atomic<bool>  g_dbg_firing{false}, g_dbg_on{false};
std::atomic<bool>  g_dbg_vis_ok{false}, g_dbg_vis_trust{false};
std::atomic<bool>  g_dbg_vis_blocked{false};
std::atomic<const char*> g_dbg_hit{"-"};
std::atomic<float> g_dbg_dist{-1.0f};
std::atomic<int>   g_dbg_weapon{0};
std::atomic<float> g_dbg_radius{-1.0f};
std::atomic<int>   g_vis_samples{0}, g_vis_hits{0};

std::thread g_thread;

double now_ms() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0).count();
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

uint32_t spotted_mask(const Memory& mem, uintptr_t pawn) {
    return mem.read<uint32_t>(
        pawn + offsets::m_entitySpottedState + offsets::m_bSpottedByMask);
}

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

// Convert a world-space radius to a screen-pixel radius at the target's
// distance. This is the fix for the fixed-pixel-radius bug: the margin now
// scales with the projection instead of being constant on screen.
float screen_radius_for(const PlayerESP& p, const Vec2& head, const Vec2& feet,
                        int sh, float world_units) {
    const float px_height = std::fabs(feet.y - head.y);
    if (px_height < 1.0f) return 1.0f;

    // A player is ~72 world units tall, so pixels-per-unit is derivable from the
    // projected height. Radius follows directly.
    const float px_per_unit = px_height / 72.0f;
    float r = world_units * px_per_unit;

    // Keep it sane at extreme distances.
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
    double vis_zero_since = 0.0;   // when the mask last started reading zero
    uintptr_t vis_zero_for = 0;    // which pawn that was for

    while (!g_stop.load()) {
        std::this_thread::yield();

        const int k = g_key.load();
        const bool gate = g_on.load() && g_mem.is_valid() && cs2_focused() &&
                          k != 0 && ((GetAsyncKeyState(k) & 0x8000) != 0);

        if (!gate) {
            if (held) { mouse_up(); held = false; }
            g_dbg_firing.store(false);
            g_dbg_on.store(false);
            continue;
        }

        const int sw = g_screen_w, sh = g_screen_h;
        if (sw <= 0 || sh <= 0) continue;

        const double now = now_ms();

        if (now - weapon_at > 250.0) {
            weapon_at = now;
            weapon_id = ESP_LocalWeaponId(g_mem, g_mem.client_dll);
            g_dbg_weapon.store(weapon_id);
        }

        const std::vector<PlayerESP> players =
            g_esp.project(g_mem, g_mem.client_dll, sw, sh, false);

        const Vec2 cross{ sw * 0.5f, sh * 0.5f };

        bool  on_target = false, vis_blocked = false;
        float best = 1e9f;
        const char* best_hit = "-";
        float shown_radius = -1.0f;

        for (const PlayerESP& p : players) {
            if (p.team == 0) continue;

            if (kTeamCheck && g_local_team != 0 && p.team == g_local_team)
                continue;

            if (!p.has_bones) continue;

            const Vec2 head = p.bones[BONE_HEAD];
            const Vec2 feet = p.screen_feet;
            const float radius = screen_radius_for(p, head, feet, sh,
                                                   kMeshRadiusUnits);
            const float head_r = screen_radius_for(p, head, feet, sh,
                                                   kHeadRadiusUnits);

            bool  this_hit = false;
            float this_d = 1e9f;
            const char* this_what = "-";

            // ---- every main wireframe link ----
            for (int i = 0; i < kBoneLinkCount; ++i) {
                const int a = kBoneLinks[i].a, b = kBoneLinks[i].b;
                if (!p.bone_ok[a] || !p.bone_ok[b]) continue;
                const float d = point_segment_dist(cross, p.bones[a],
                                                   p.bones[b]);
                if (d > radius) continue;
                if (d < this_d) { this_d = d; this_what = "body"; this_hit = true; }
            }

            // ---- extra links: shoulders, clavicles, spine_0, hands ----
            for (int i = 0; i < kExtraLinkCount; ++i) {
                const int a = kExtraLinks[i].a, b = kExtraLinks[i].b;
                if (!p.bone_ok[a] || !p.bone_ok[b]) continue;
                const float d = point_segment_dist(cross, p.bones[a],
                                                   p.bones[b]);
                if (d > radius) continue;
                if (d < this_d) { this_d = d; this_what = "body"; this_hit = true; }
            }

            // ---- head sphere ----
            if (p.bone_ok[BONE_HEAD]) {
                const float dx = head.x - cross.x, dy = head.y - cross.y;
                const float d = std::sqrt(dx * dx + dy * dy);
                if (d <= head_r) {
                    if (kPreferHead || d < this_d) {
                        this_d = d;
                        this_what = "head";
                        this_hit = true;
                    }
                }
            }

            if (!this_hit) continue;

            // ---- vis check: SOFT gate ----
            // Only blocks after SUSTAINED zero. A brief appearance is never
            // blocked, which is what fixes the tight-angle misses.
            bool blocked_here = false;
            if (kVisCheck && p.pawn) {
                const uint32_t mask = spotted_mask(g_mem, p.pawn);
                ++g_vis_samples;
                if (mask != 0) {
                    ++g_vis_hits;
                    if (g_vis_hits >= kVisTrustSamples) vis_trusted = true;
                }

                if (vis_trusted) {
                    if (mask == 0) {
                        if (vis_zero_for != p.pawn || vis_zero_since <= 0.0) {
                            vis_zero_for = p.pawn;
                            vis_zero_since = now;
                        } else if ((now - vis_zero_since) >= kVisBlockAfterMs) {
                            blocked_here = true;
                        }
                    } else {
                        vis_zero_for = 0;
                        vis_zero_since = 0.0;
                    }
                }
            }

            if (blocked_here) {
                vis_blocked = true;
                continue;
            }

            on_target = true;
            if (this_d < best) {
                best = this_d;
                best_hit = this_what;
                shown_radius = radius;
            }
        }

        g_dbg_on.store(on_target);
        g_dbg_vis_ok.store(on_target);
        g_dbg_vis_trust.store(vis_trusted);
        g_dbg_vis_blocked.store(vis_blocked);
        g_dbg_dist.store(on_target ? best : -1.0f);
        g_dbg_hit.store(on_target ? best_hit : "-");
        g_dbg_radius.store(shown_radius);

        const int interval = weapon_interval(weapon_id);
        const int clamped  = interval < kMinIntervalMs ? kMinIntervalMs : interval;

        if (kFiring && on_target) {
            if (!held) {
                mouse_down();
                held = true;
                next_shot = now + clamped;
                g_dbg_firing.store(true);
            } else if (now >= next_shot) {
                mouse_up();
                mouse_down();
                next_shot = now + clamped;
            }
        } else if (held) {
            mouse_up();
            held = false;
            g_dbg_firing.store(false);
        }
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
    d.enabled      = g_on.load();
    d.firing       = g_dbg_firing.load();
    d.on_target    = g_dbg_on.load();
    d.vis_usable   = g_dbg_vis_trust.load();
    d.vis_blocked  = g_dbg_vis_blocked.load();
    d.vis_samples  = g_vis_samples.load();
    d.vis_hits     = g_vis_hits.load();
    d.target_hit   = g_dbg_hit.load();
    d.target_dist  = g_dbg_dist.load();
    d.mesh_radius  = g_dbg_radius.load();
    d.weapon_id    = g_dbg_weapon.load();
    return d;
}

void Aim_SetEnabled(bool on) { g_on.store(on); }
bool Aim_Enabled() { return g_on.load(); }
void Aim_SetKey(int vk) { g_key.store(vk); }
int  Aim_Key() { return g_key.load(); }
