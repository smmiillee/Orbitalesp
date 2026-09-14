// --- src/movement.h ---
#pragma once
#include <cstdint>

// Jumpbug. CTRL injection only, no memory writes. Independent of bhop.
//
// MECHANIC: hold crouch during the fall, then RELEASE it as you touch down.
// The release at touchdown is what produces the bug. Nothing else about the
// timing matters much -- the release moment is the whole trick.
//
// SO THE DEFAULT IS: uncrouch lead 0, meaning "release when the ground flag
// says we landed". An earlier version released `uncrouch_lead` ms BEFORE the
// predicted impact, which meant the release happened mid-air and there was no
// release left to happen at touchdown -- the crouch was already up. That is why
// it did nothing while the counter still climbed.
//
// Raise `uncrouch_lead` only to test releasing slightly early.
struct MovementDebug {
    bool  enabled   = false;
    bool  on_ground = false;
    bool  crouching = false;
    bool  armed     = false;
    float vz        = 0.0f;
    float tti       = -1.0f;
    int   jumpbugs  = 0;   // crouch events started
};

void Movement_Init();
void Movement_Shutdown();

MovementDebug Movement_GetDebug();

void Movement_SetJumpbug(bool on);
bool Movement_Jumpbug();

void Movement_SetKey(int vk);
int  Movement_Key();

// Press crouch when tti drops below this.
void  Movement_SetCrouchLead(float ms);
float Movement_CrouchLead();

// Release crouch when tti drops below this. 0 = release on the ground flag,
// which is the normal jumpbug.
void  Movement_SetUncrouchLead(float ms);
float Movement_UncrouchLead();
