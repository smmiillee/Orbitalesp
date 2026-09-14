// --- src/aim.h ---
#pragma once
#include <cstdint>

// Triggerbot.
//
// DETECTION is read-only: it uses the player and bone reads the ESP already
// performs. FIRING is not read-only and cannot be -- the game only shoots when
// it receives a click, so firing injects a mouse button press. Untick Firing
// to leave it detection-only.
struct AimDebug {
    bool  enabled     = false;
    bool  firing      = false;
    bool  on_target   = false;
    float target_dist = 0.0f;
    const char* target_bone = "-";
};

void Aim_Init();
void Aim_Shutdown();

AimDebug Aim_GetDebug();

void Aim_SetEnabled(bool on);
bool Aim_Enabled();

void Aim_SetFire(bool on);
bool Aim_Fire();

// Arm key: the trigger only acts while this is held. 0 = always active.
void Aim_SetKey(int vk);
int  Aim_Key();

// Trigger radius, as a percentage of screen height.
void  Aim_SetRadius(float percent_of_height);
float Aim_Radius();
