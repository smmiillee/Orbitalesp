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
// HARDCODED BEHAVIOUR -- no toggles for these, they are always on.
// ===========================================================================
constexpr bool kTeamCheck = true;
constexpr bool kVisCheck  = true;
constexpr bool kFiring    = true;

// Radius of the mesh tube, authored at 1080p and scaled by height. Applied to
// every segment, so it is the thickness of the wireframe.
constexpr float kMeshRadiusAt1080 = 9.0f;
constexpr float kReferenceHeight  = 1080.0f;

// The head is a sphere rather than a link, so it gets a slightly tighter test.
constexpr float kHeadScale = 0.6f;

// If the spotted mask has been sampled this many times without EVER reading
// nonzero, the offset is wrong for this build and the check stops blocking.
constexpr int kVisGiveUpSamples = 400;

std::atomic<bool> g_stop{false}, g_started{false};
std::atomic<bool> g_on{false};
std::atomic<int>  g_key{0};       // 0 == unbound == OFF

std::atomic<bool>  g_dbg_firing{false}, g_dbg_on{false};
std::atomic<bool>  g_dbg_vis{false},    g_dbg_visable{false};
std::atomic<const char*> g_dbg_hit_bone{"-"};
std::atomic<const char*> g_dbg_blocked{"-"};
std::atomic<float> g_dbg_dist{-1.0f};
std::atomic<int>   g_dbg_weapon{0};
std::atomic<int>   g_vis_samples{0}, g_vis_hits{0};

std::thread g_thread;

// ---- per-weapon shot interval, ms ----
// Cycle times, so the trigger paces itself to the gun. Full-auto weapons are
// pulsed at their own rate, which is equivalent to holding but lets one code
// path serve every weapon type.
struct WeaponTiming { int id; int interval_ms; };
constexpr WeaponTiming kTimings[] = {
    {  1, 267 },  // Deagle
    {  2, 120 },  // Dual Berettas
    {  3, 150 },  // Five-SeveN
    {  4, 150 },  // Glock-18
    {  7, 100 },  // AK-47
    {  8,  90 },  // AUG
    {  9, 1460 }, // AWP
    { 10,  90 },  // FAMAS
    { 11, 250 },  // G3SG1
    { 13,  90 },  // Galil AR
    { 14,  80 },  // M249
    { 16,  90 },  // M4A4
    { 17,  70 },  // MAC-10
    { 19,  70 },  // P90
    { 23,  80 },  // MP5-SD
    { 24,  90 },  // UMP-45
    { 25, 250 },  // XM1014
    { 26,  70 },  // PP-Bizon
    { 27, 850 },  // MAG-7
    { 28,  70 },  // Negev
    { 29, 850 },  // Sawed-Off
    { 30, 120 },  // Tec-9
    { 31, 400 },  // Zeus
    { 32, 150 },  // P2000
    { 33,  80 },  // MP7
    { 34,  70 },  // MP9
    { 35, 850 },  // Nova
    { 36, 150 },  // P250
    { 38, 250 },  // SCAR-20
    { 39,  90 },  // SG 553
    { 40, 1250 }, // SSG 08
    { 60, 100 },  // M4A1-S
    { 61, 150 },  // USP-S
    { 63,  70 },  // CZ75-Auto
    { 64, 350 },  // R8 Revolver
};

constexpr int kDefaultIntervalMs = 120;
constexpr int kMinIntervalMs     = 60;

int weapon_interval(int id) {
    for (const WeaponTiming& w : kTimings)
        if (w.id == id) return w.interval_ms;
    return kDefaultIntervalMs;
}

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

// Radar-spotted mask. APPROXIMATE visibility -- see aim.h.
uint32_t spotted_mask(const Memory& mem, uintptr_t pawn) {
    return mem.read<uint32_t>(
        pawn + offsets::m_entitySpottedState + offsets::m_bSpottedByMask);
}

// 2D distance from the crosshair to the LINE between two projected joints,
// clamped to the endpoints. This is the mesh test: it covers the whole limb
// rather than just the joints at either end.
float point_segment_dist(const Vec2& p, const Vec2& a, const Vec2& b) {
    const float vx = b.x - a.x;
    const float vy = b.y - a.y;
    const float wx = p.x - a.x;
    const float wy = p.y - a.y;

    const float len2 = vx * vx + vy * vy;
    if (len2 < 0.0001f) {
        const float dx = p.x - a.x, dy = p.y - a.y;
        return std::sqrt(dx * dx + dy * dy);
    }

    float t = (wx * vx + wy * vy) / len2;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;

    const float px = a.x + vx * t;
    const float py = a.y + vy * t;
    const float dx = p.x - px, dy = p.y - py;
    return std::sqrt(dx * dx + dy * dy);
}

void run_thread() {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);

    bool   held = false;
    double next_shot = 0.0;

    int    weapon_id = 0;
    double weapon_at = -1e9;

    // Until the mask has read nonzero at least once, the vis check is not
    // trusted and does not block. That is what keeps a wrong offset from
    // silently refusing every shot.
    bool vis_trusted = false;

    while (!g_stop.load()) {
        std::this_thread::yield();

        const int k = g_key.load();

        // UNBOUND MEANS OFF.
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

        // The weapon can change between rounds, so re-read it ~4x/sec.
        if (now - weapon_at > 250.0) {
            weapon_at = now;
            weapon_id = ESP_LocalWeaponId(g_mem, g_mem.client_dll);
            g_dbg_weapon.store(weapon_id);
        }

        // Newest sample, no interpolation: a smoothed position would aim behind
        // a moving target.
        const std::vector<PlayerESP> players =
            g_esp.project(g_mem, g_mem.client_dll, sw, sh, false);

        const Vec2 cross{ sw * 0.5f, sh * 0.5f };
        const float radius = kMeshRadiusAt1080 * ((float)sh / kReferenceHeight);

        bool  on_target = false, blocked = false;
        float best = 1e9f;
        const char* best_bone = "-";
        const char* why = "-";

        for (const PlayerESP& p : players) {
            if (p.team == 0) continue;

            // team check: hardcoded on
            if (kTeamCheck && g_local_team != 0 && p.team == g_local_team)
                continue;

            if (!p.has_bones) continue;

            // ---- mesh: every link of the wireframe ----
            bool this_hit = false;
            const char* this_bone = "-";
            float this_d = 1e9f;

            for (int i = 0; i < kBoneLinkCount; ++i) {
                const int a = kBoneLinks[i].a;
                const int b = kBoneLinks[i].b;
                if (!p.bone_ok[a] || !p.bone_ok[b]) continue;

                const float d = point_segment_dist(cross, p.bones[a],
                                                   p.bones[b]);
                if (d > radius) continue;
                if (d < this_d) { this_d = d; this_bone = "link"; this_hit = true; }
            }

            // ---- head: a sphere, so its own test ----
            if (p.bone_ok[BONE_HEAD]) {
                const float dx = p.bones[BONE_HEAD].x - cross.x;
                const float dy = p.bones[BONE_HEAD].y - cross.y;
                const float d = std::sqrt(dx * dx + dy * dy) -
                                radius * (1.0f - kHeadScale);
                if (d <= radius && d < this_d) {
                    this_d = d;
                    this_bone = "head";
                    this_hit = true;
                }
            }

            if (!this_hit) continue;

            // ---- vis check: hardcoded on, self-disabling ----
            if (kVisCheck && p.pawn) {
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
                        continue;   // visible candidate, but hidden
                    }
                }
            }

            on_target = true;
            if (this_d < best) {
                best = this_d;
                best_bone = this_bone;
            }
        }

        g_dbg_on.store(on_target);
        g_dbg_vis.store(on_target);
        g_dbg_visable.store(vis_trusted);
        g_dbg_dist.store(on_target ? best : -1.0f);
        g_dbg_hit_bone.store(on_target ? best_bone : "-");
        g_dbg_blocked.store(blocked ? why : "-");

        // ---- fire at the weapon's own cycle rate ----
        const int interval = weapon_interval(weapon_id);
        const int clamped  = interval < kMinIntervalMs ? kMinIntervalMs : interval;

        if (kFiring && on_target) {
            if (!held) {
                mouse_down();
                held = true;
                next_shot = now + clamped;
                g_dbg_firing.store(true);
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
    d.enabled     = g_on.load();
    d.firing      = g_dbg_firing.load();
    d.on_target   = g_dbg_on.load();
    d.has_vis     = g_dbg_vis.load();
    d.vis_usable  = g_dbg_visable.load();
    d.vis_samples = g_vis_samples.load();
    d.vis_hits    = g_vis_hits.load();
    d.blocked_by  = g_dbg_blocked.load();
    d.target_bone = g_dbg_hit_bone.load();
    d.target_dist = g_dbg_dist.load();
    d.weapon_id   = g_dbg_weapon.load();
    return d;
}

void Aim_SetEnabled(bool on) { g_on.store(on); }
bool Aim_Enabled() { return g_on.load(); }
void Aim_SetKey(int vk) { g_key.store(vk); }
int  Aim_Key() { return g_key.load(); }
