// --- src/movement.h ---
#pragma once
#include <cstdint>

// Jumpbug. SPACE + CTRL injection only, no memory writes. Independent of bhop.
//
// ══ TIMING, AND WHY IT IS NOT A MILLISECOND VALUE ═══════════════════════
// The window is a HEIGHT, not a delay. Per zer0k-z/cs2-movement-issues, a
// crouchbug (and therefore a jumpbug) can only be performed when the player is
// 9 to 11 units ABOVE THE GROUND. Timing this in ms is meaningless because the
// window in ms depends on fall speed.
//
// Sequence, matching the reference jumpbug implementations:
//   1. crouch ~2 ticks before landing
//   2. release crouch when 9-11 units above the ground
//   3. press jump at that same moment
//
// The uncrouch-then-jump ORDER is what matters: the reference CS:GO alias is
// "-duck; +jump 1; -jump 1" and the SourceRuns wiki describes it as "instantly
// uncrouch and jump".
//
// ══ KEYBIND REQUIRED ════════════════════════════════════════════════════
// There is no "always on". An unbound key means the feature is OFF. Click set,
// press a key, then hold that key to enable.
struct MovementDebug {
    bool  enabled     = false;
    bool  armed       = false;   // gate held and acting
    bool  on_ground   = false;
    bool  crouching   = false;
    bool  game_ducked = false;   // the GAME's own crouch flag
    bool  jumping     = false;
    float vz          = 0.0f;
    float height      = -1.0f;   // units above the last standing height
    float tti         = -1.0f;   // predicted ms to landing, -1 unknown
    int   jumpbugs    = 0;
};

void Movement_Init();
void Movement_Shutdown();

MovementDebug Movement_GetDebug();

void Movement_SetJumpbug(bool on);
bool Movement_Jumpbug();

// 0 means UNBOUND, which means the feature does not run at all.
void Movement_SetKey(int vk);
int  Movement_Key();

// How early to crouch, in ms before the predicted landing. 2 ticks is ~31 ms.
void  Movement_SetCrouchLead(float ms);
float Movement_CrouchLead();

// The uncrouch window, in units above the last standing height. The documented
// window is 9-11; the low edge is the trigger.
void  Movement_SetUncrouchHeight(float units);
float Movement_UncrouchHeight();
