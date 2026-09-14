// --- src/movement.cpp ---
// Jumpbug: crouch during the fall, then uncrouch + jump 9-11 units above the
// ground. SPACE and CTRL injection only, no memory writes.
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

// The documented jumpbug window, in units above the ground. Triggered on ENTRY
// (<=) so the input has the whole window to land in.
constexpr float kUncrouchHeight = 11.0f;

// Crouch two ticks before the predicted landing, matching the reference.
constexpr float kDefaultCrouchLeadMs = 31.0f;

constexpr float kMinFallSpeed = 40.0f;
constexpr float kMaxTtiMs     = 1500.0f;
constexpr double kJumpHoldMs  = 12.0;

// How long Z must be motionless before we even consider "on ground".
constexpr double kStillMs = 22.0;
// ...and how close to the last ground height Z must be, which is what excludes
// the APEX of a jump. At apex Z is barely moving, so a stillness test alone
// reads it as grounded.
constexpr float kGroundBand = 3.0f;

std::atomic<bool>  g_stop{false}, g_started{false};
std::atomic<bool>  g_on{false};
std::atomic<int>   g_key{0};                   // 0 == unbound == OFF
std::atomic<float> g_crouch_lead{kDefaultCrouchLeadMs};
std::atomic<float> g_uncrouch_h{kUncrouchHeight};

std::atomic<bool>  g_dbg_ground{false}, g_dbg_crouch{false};
std::atomic<bool>  g_dbg_armed{false},  g_dbg_ducked{false};
std::atomic<bool>  g_dbg_jumping{false};
std::atomic<float> g_dbg_vz{0.0f}, g_dbg_height{-1.0f}, g_dbg_tti{-1.0f};
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

void key_down(int vk) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk;
    in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    SendInput(1, &in, sizeof(INPUT));
}

void key_up(int vk) {
    INPUT in{};
    in.type = INPUT_KEYBOARD;
    in.ki.wVk = (WORD)vk;
    in.ki.wScan = (WORD)MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    in.ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(1, &in, sizeof(INPUT));
}

struct Watch {
    bool   has_prev = false;
    float  prev_z = 0.0f;
    double prev_t = 0.0;
    double still_since = 0.0;
    float  vz = 0.0f;

    // Running ground reference. Drops immediately if we end up lower, rises
    // only when we are confidently standing.
    float  ground_ref = 0.0f;
    bool   have_ref = false;

    bool   on_ground = false;
    float  height = -1.0f;
    float  tti = -1.0f;

    // Grading for the flag, so it is only trusted once it has PROVEN itself.
    int    flag_g = 0, flag_a = 0;
    bool   flag_ok = false;
};
Watch g_w;

void update_watch(const Memory& mem, uintptr_t pawn) {
    const double now = now_ms();

    const float z = mem.read<float>(pawn + offsets::m_vOldOrigin + 8);
    const uint32_t flags = mem.read<uint32_t>(pawn + offsets::m_fFlags);
    const uint32_t hge   = mem.read<uint32_t>(pawn + offsets::m_hGroundEntity);

    const bool flag_bit = (flags & offsets::kFlagOnGround) != 0u;

    g_dbg_ducked.store((flags & offsets::kFlagDucking) != 0u);

    if (!g_w.has_prev) {
        g_w.has_prev = true;
        g_w.prev_z = z;
        g_w.prev_t = now;
        g_w.still_since = now;
        g_w.ground_ref = z;
        g_w.have_ref = true;
        return;
    }

    const float dz = z - g_w.prev_z;
    const double dt = (now - g_w.prev_t) / 1000.0;

    if (std::fabs(dz) >= 0.5f) g_w.still_since = now;
    const bool z_still = (now - g_w.still_since) > kStillMs;

    // Velocity from distinct samples only, so the estimate does not spike.
    if (!g_w.on_ground && dt > 0.0005 && dt < 0.25 &&
        std::fabs(dz) >= 0.05f) {
        const float v = (float)(dz / dt);
        g_w.vz = g_w.vz * 0.4f + v * 0.6f;
    }

    g_w.prev_z = z;
    g_w.prev_t = now;

    // *** GROUND DETECTION, AND WHY THE OLD ONE NEVER FIRED ***
    // The previous version trusted m_fFlags bit 0 directly:
    //     ground = (flags & 1) || (z_still && slow)
    // But bit 0 is NOT FL_ONGROUND on the local predicted pawn (it reads
    // 0x10000 while standing). If that bit happened to read SET while airborne,
    // ground was permanently true -- so the crouch never fired and the jump
    // never fired, which is exactly "crouch isn't working either".
    //
    // Now: Z-based primary, with a band test that excludes the APEX (where Z is
    // briefly motionless but we are ~55 units up), and the flag used only after
    // it has been observed BOTH set while grounded and clear while airborne.
    if (z < g_w.ground_ref) g_w.ground_ref = z;   // fell to a lower level

    const bool near_ref = (z - g_w.ground_ref) <= kGroundBand;
    const bool z_ground = z_still && near_ref;

    if (z_ground) {
        if (flag_bit) ++g_w.flag_g;
    } else {
        if (!flag_bit) ++g_w.flag_a;
    }
    if (!g_w.flag_ok && g_w.flag_g >= 8 && g_w.flag_a >= 8) g_w.flag_ok = true;

    const bool hge_ground = (hge != offsets::kGroundEntityNone) &&
                            (hge == 0x8000u);

    bool ground;
    if (g_w.flag_ok) ground = flag_bit;
    else             ground = z_ground;
    if (!ground && hge_ground) ground = true;

    g_w.on_ground = ground;

    if (ground) {
        g_w.ground_ref = z;      // reference follows the surface we stand on
        g_w.vz = 0.0f;
        g_w.height = 0.0f;
        g_w.tti = -1.0f;
    } else {
        g_w.height = z - g_w.ground_ref;
        if (g_w.vz < -kMinFallSpeed && g_w.height > 0.0f) {
            const float t = (g_w.height / -g_w.vz) * 1000.0f;
            g_w.tti = (t > kMaxTtiMs) ? -1.0f : t;
        } else {
            g_w.tti = -1.0f;
        }
    }

    g_dbg_ground.store(g_w.on_ground);
    g_dbg_vz.store(g_w.vz);
    g_dbg_height.store(g_w.height);
    g_dbg_tti.store(g_w.tti);
}

void run_thread() {
    uintptr_t pawn = 0;
    double pawn_at = 0.0;

    bool crouching = false;
    bool fired = false;
    bool jump_down = false;
    double jump_at = 0.0;

    while (!g_stop.load()) {
        wait_ms(1.0);

        const int key = g_key.load();

        // UNBOUND MEANS OFF: the feature runs only while a bound key is held.
        const bool gate = g_on.load() && cs2_focused() && key != 0 &&
                          ((GetAsyncKeyState(key) & 0x8000) != 0);

        if (!g_mem.is_valid() || !gate) {
            if (crouching) { key_up(VK_CONTROL); crouching = false; }
            if (jump_down) { key_up(VK_SPACE);   jump_down = false; }
            fired = false;
            g_dbg_crouch.store(false);
            g_dbg_armed.store(false);
            g_dbg_jumping.store(false);
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

        if (jump_down && (now - jump_at) >= kJumpHoldMs) {
            key_up(VK_SPACE);
            jump_down = false;
        }

        if (g_w.on_ground) {
            if (crouching) { key_up(VK_CONTROL); crouching = false; }
            fired = false;
            g_dbg_crouch.store(false);
            g_dbg_armed.store(false);
            continue;
        }

        const float height = g_w.height;
        const float tti    = g_w.tti;
        const bool  falling = (g_w.vz < -kMinFallSpeed);

        // 1. crouch ~2 ticks before landing
        if (!crouching && !fired && falling &&
            tti >= 0.0f && tti <= g_crouch_lead.load()) {
            key_down(VK_CONTROL);
            crouching = true;
            g_dbg_n.fetch_add(1);
        }

        // 2. uncrouch + jump on entering the 9-11 unit window.
        //    Order is uncrouch THEN jump, per the reference implementations.
        if (crouching && !fired && falling &&
            height >= 0.0f && height <= g_uncrouch_h.load()) {
            key_up(VK_CONTROL);
            crouching = false;
            key_down(VK_SPACE);
            jump_down = true;
            jump_at = now;
            fired = true;
            g_dbg_jumping.store(true);
        }

        // 3. fallback: if the window was missed, still release and jump at
        //    touchdown. Usually too late for the height gain, but it preserves
        //    the crouch release that negates fall damage.
        if (crouching && !fired && height < 0.0f) {
            key_up(VK_CONTROL);
            crouching = false;
            key_down(VK_SPACE);
            jump_down = true;
            jump_at = now;
            fired = true;
            g_dbg_jumping.store(true);
        }

        g_dbg_crouch.store(crouching);
        g_dbg_armed.store(true);
    }

    if (crouching) key_up(VK_CONTROL);
    if (jump_down) key_up(VK_SPACE);
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
    d.enabled     = g_on.load();
    d.armed       = g_dbg_armed.load();
    d.on_ground   = g_dbg_ground.load();
    d.crouching   = g_dbg_crouch.load();
    d.game_ducked = g_dbg_ducked.load();
    d.jumping     = g_dbg_jumping.load();
    d.vz          = g_dbg_vz.load();
    d.height      = g_dbg_height.load();
    d.tti         = g_dbg_tti.load();
    d.jumpbugs    = g_dbg_n.load();
    return d;
}

void Movement_SetJumpbug(bool on) { g_on.store(on); }
bool Movement_Jumpbug() { return g_on.load(); }
void Movement_SetKey(int vk) { g_key.store(vk); }
int  Movement_Key() { return g_key.load(); }

void Movement_SetCrouchLead(float ms) {
    if (ms < 4.0f)   ms = 4.0f;
    if (ms > 200.0f) ms = 200.0f;
    g_crouch_lead.store(ms);
}
float Movement_CrouchLead() { return g_crouch_lead.load(); }

void Movement_SetUncrouchHeight(float units) {
    if (units < 2.0f)  units = 2.0f;
    if (units > 40.0f) units = 40.0f;
    g_uncrouch_h.store(units);
}
float Movement_UncrouchHeight() { return g_uncrouch_h.load(); }
