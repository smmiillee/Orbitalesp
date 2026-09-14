// --- src/movement.h ---
#pragma once
#include <cstdint>

// Jumpbug. CTRL injection only, no memory writes. Independent of bhop.
//
// Crouch during the fall, then uncrouch just before touchdown. The uncrouch is
// what produces the bug.
//
// GROUND DETECTION: m_fFlags bit 0 is the primary signal because it is
// instance-accurate. A Z-stillness test is only a fallback, and only when
// vertical velocity is also near zero -- a Z-stillness test on its own reads
// the APEX of a jump as "grounded" (Z barely moves there), which corrupts the
// reference height and makes the uncrouch fire early.
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

void Movement_SetKey(int vk);
int  Movement_Key();

// Crouch this many ms before the predicted landing.
void  Movement_SetCrouchLead(float ms);
float Movement_CrouchLead();

// Uncrouch this many ms before the predicted landing. This is the bug window.
void  Movement_SetUncrouchLead(float ms);
float Movement_UncrouchLead();
