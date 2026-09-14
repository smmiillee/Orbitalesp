// --- src/bhop.cpp ---
//
// Two bhop modes, zero memory writes:
//
// MODE 0 — Standard scroll:
//   Reads m_fFlags for ground detection (bit 0 = FL_ONGROUND).
//   On landing: fires scroll-down via SendInput.
//   timeBeginPeriod(1) sets Windows timer to 1ms so sleep calls are precise.
//   Requires: bind mwheeldown "+jump" in CS2 console.
//
// MODE 1 — 64fps scroll:
//   Same as Mode 0 but also sends "fps_max 64" to CS2 on enable
//   and "fps_max 0" on disable via the CS2 console command interface.
//   Locks frames to subtick boundaries — most consistent timing.
//   Requires: bind mwheeldown "+jump" in CS2 console.
//
#include "bhop.h"
#include "memory.h"
#include "offsets.h"
#include <Windows.h>
#include <mmsystem.h>
#include <thread>
#include <chrono>
#include <string>

#pragma comment(lib, "winmm.lib")

// Bhop config — written by main.cpp menu
BhopConfig g_bhop_cfg;

// ── helpers ──────────────────────────────────────────────────────────────────

static void scroll_down() {
    INPUT inp{};
    inp.type         = INPUT_MOUSE;
    inp.mi.dwFlags   = MOUSEEVENTF_WHEEL;
    inp.mi.mouseData = static_cast<DWORD>(-WHEEL_DELTA);
    SendInput(1, &inp, sizeof(INPUT));
}

// Send a CS2 console command by finding the window and using WM_CHAR
static void cs2_console_cmd(const std::string& cmd) {
    HWND cs2 = FindWindowA("SDL_app", nullptr);
    if (!cs2) return;

    // Open console
    PostMessage(cs2, WM_KEYDOWN, VK_OEM_3, 0); // ` key
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Type command
    for (char c : cmd)
        PostMessage(cs2, WM_CHAR, c, 0);

    // Enter
    PostMessage(cs2, WM_KEYDOWN, VK_RETURN, 0);
    std::this_thread::sleep_for(std::chrono::milliseconds(30));

    // Close console
    PostMessage(cs2, WM_KEYDOWN, VK_OEM_3, 0);
}

// ── state ─────────────────────────────────────────────────────────────────────

static bool s_timer_set    = false;
static bool s_fps_locked   = false;
static bool s_was_enabled  = false;
static int  s_prev_mode    = -1;

static void ensure_timer() {
    if (!s_timer_set) {
        timeBeginPeriod(1); // 1ms Windows timer resolution
        s_timer_set = true;
    }
}

static void apply_fps_lock(bool lock) {
    if (lock && !s_fps_locked) {
        cs2_console_cmd("fps_max 64");
        s_fps_locked = true;
    } else if (!lock && s_fps_locked) {
        cs2_console_cmd("fps_max 0");
        s_fps_locked = false;
    }
}

// ── main tick ────────────────────────────────────────────────────────────────

void BhopTick() {
    if (!g_bhop_cfg.enabled) {
        // Cleanup on disable
        if (s_was_enabled) {
            apply_fps_lock(false);
            s_was_enabled = false;
        }
        return;
    }

    ensure_timer();

    // Handle mode changes and enable transitions
    bool mode64 = (g_bhop_cfg.mode == 1);
    if (!s_was_enabled || s_prev_mode != g_bhop_cfg.mode) {
        apply_fps_lock(mode64);
        s_was_enabled = true;
        s_prev_mode   = g_bhop_cfg.mode;
    }

    if (!(GetAsyncKeyState(VK_SPACE) & 0x8000)) return;

    // CS2 must be foreground
    HWND cs2 = FindWindowA("SDL_app", nullptr);
    if (!cs2 || GetForegroundWindow() != cs2) return;

    uintptr_t local_pawn = g_mem.read<uintptr_t>(
        g_mem.base_address + offsets::dwLocalPlayerPawn);
    if (!local_pawn) return;

    // m_fFlags bit 0 = FL_ONGROUND — more reliable than m_hGroundEntity
    uint32_t flags = g_mem.read<uint32_t>(local_pawn + offsets::m_fFlags);
    bool on_ground = (flags & 1);

    static bool s_was_in_air = false;

    if (s_was_in_air && on_ground) {
        // Just landed — fire jump scroll immediately
        scroll_down();
        // Second tap after 6ms to cover subtick jitter
        std::this_thread::sleep_for(std::chrono::milliseconds(6));
        scroll_down();
    }

    s_was_in_air = !on_ground;
}
