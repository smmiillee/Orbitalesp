// --- src/movement.h ---
#pragma once
#include <cstdint>

// Jumpbug. CTRL injection only.
//
// Crouch just before landing, hold through touchdown, release after. The
// damage-negation window is wide; the height-gain window is narrow.
struct MovementDebug {
    bool  enabled   = false;
    bool  on_ground = false;
    bool  crouching = false;
    bool  armed     = false;
    float vz        = 0.0f;
    float tti       = -1.0f;
    int   jumpbugs  = 0;
};

void Movement_Init();
void Movement_Shutdown();

MovementDebug Movement_GetDebug();

void Movement_SetJumpbug(bool on);
bool Movement_Jumpbug();

// Key that arms the jumpbug. 0 = always armed.
void Movement_SetKey(int vk);
int  Movement_Key();
