// --- src/movement.h ---
#pragma once
#include <cstdint>

// Jumpbug. CTRL injection only, no memory writes. Independent of bhop.
//
// Crouch during the fall, release as you touch down. The release AT touchdown
// is the bug, so the default releases on the ground flag.
//
// This now also READS the player's crouch state (m_bDucking via m_fFlags bit 2)
// so you can see whether the crouch we injected actually reached the game.
// Without that we cannot tell "our timing is wrong" from "the game never
// registered the crouch at all", which look identical from the outside.
struct MovementDebug {
    bool  enabled     = false;
    bool  on_ground   = false;
    bool  crouching   = false;   // we have CTRL held
    bool  game_ducked = false;   // the GAME thinks we are crouched
    bool  armed       = false;
    bool  jumping     = false;   // SPACE currently held
    float vz          = 0.0f;
    float tti         = -1.0f;
    int   jumpbugs    = 0;
};

void Movement_Init();
void Movement_Shutdown();

MovementDebug Movement_GetDebug();

void Movement_SetJumpbug(bool on);
bool Movement_Jumpbug();

void Movement_SetKey(int vk);
int  Movement_Key();

void  Movement_SetCrouchLead(float ms);
float Movement_CrouchLead();

void  Movement_SetUncrouchLead(float ms);
float Movement_UncrouchLead();
