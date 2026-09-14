// --- src/aim.h ---
#pragma once
#include <cstdint>

// Triggerbot.
//
// ══ READ THIS: WHAT "READ ONLY" MEANS HERE ══════════════════════════════
// DETECTION is read-only -- it works entirely from the player and bone reads
// the ESP already performs. Nothing is written to cs2.exe.
//
// FIRING is not read-only and cannot be: the game only shoots when it receives
// a click, so the trigger injects a mouse button press. That is input
// injection, the same class of action bhop uses for space. There is no way to
// make a triggerbot shoot from a read alone.
//
// If you want zero injection, turn the Firing toggle off: the trigger still
// detects and can flash an indicator, it just never clicks.
//
// ══ HOW IT DECIDES ══════════════════════════════════════════════════════
// The crosshair is always at the centre of the screen. So the test is simply:
// does any enemy bone project within `radius` pixels of screen centre? The
// bones come from the ESP, and the trigger asks for the NEWEST sample rather
// than the interpolated one -- a 35 ms-stale position would mean firing behind
// a moving target.
struct AimDebug {
    bool  enabled     = false;
    bool  firing      = false;
    bool  on_target   = false;
    float target_dist = 0.0f;   // screen px from crosshair to nearest bone
    const char* target_bone = "-";
};

void Aim_Init();
void Aim_Shutdown();

AimDebug Aim_GetDebug();

void Aim_SetEnabled(bool on);
bool Aim_Enabled();

// Firing itself can be disabled, leaving detection only.
void Aim_SetFire(bool on);
bool Aim_Fire();

// Aim key: only trigger while this key is held. 0 = always active.
void Aim_SetKey(int vk);
int  Aim_Key();
