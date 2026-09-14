// --- src/aim.h ---
#pragma once
#include <cstdint>

// Triggerbot.
//
// DETECTION is read-only. FIRING is not and cannot be -- the game only shoots
// when it receives a click, so firing injects a mouse button press. Untick
// Firing to leave it detection-only.
struct AimDebug {
    bool  enabled     = false;
    bool  firing      = false;
    bool  on_target   = false;
    float target_dist = 0.0f;
    const char* target_bone = "-";
    int   delay_ms    = 0;      // configured delay
    int   held_ms     = 0;      // how long the current target has been held
};

void Aim_Init();
void Aim_Shutdown();

AimDebug Aim_GetDebug();

void Aim_SetEnabled(bool on);
bool Aim_Enabled();

void Aim_SetFire(bool on);
bool Aim_Fire();

void Aim_SetKey(int vk);
int  Aim_Key();

void  Aim_SetRadius(float percent_of_height);
float Aim_Radius();

// Wait this long after acquiring a target before firing, 0-200 ms.
void Aim_SetDelay(int ms);
int  Aim_Delay();
