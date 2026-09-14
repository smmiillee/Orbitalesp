// --- src/movement.h ---
#pragma once
#include <cstdint>

// Movement features. SPACE/CTRL injection only, no memory writes.
//
// ══ JUMPBUG ═════════════════════════════════════════════════════════════
// Crouching in the moment before you land causes the game to resolve the
// landing as a jump, which keeps your horizontal velocity and, on the frames it
// lines up, gains a little height and negates fall damage.
//
// For that to fire we have to crouch BEFORE touchdown, which means predicting
// the landing from vertical velocity -- the same prediction the bhop uses.
//
// TWO WINDOWS, AND WHY ONLY ONE OF THEM IS HARD:
//   * the HEIGHT GAIN needs the crouch to land in a narrow frame window, so it
//     is exactly as timing-sensitive as the bhop and shares its limit.
//   * the FALL DAMAGE NEGATION is far more forgiving -- the crouch only has to
//     be down somewhere near the landing, not in one exact frame.
//
// So this reliably cancels fall damage and will hit the height gain often, but
// it cannot be guaranteed frame-exact from outside the process, for the same
// reason the bhop cannot.
//
// ══ HOW IT WORKS ════════════════════════════════════════════════════════
// While airborne and falling, we watch time-to-impact. When it drops inside
// `lead` we press CTRL and HOLD it through the landing, then release a short
// time after touchdown. Holding across the landing is what makes the forgiving
// window work: the crouch is present for every frame around the impact rather
// than one frame we hoped was right.
//
// It runs off the same held-key gate style as bhop: hold SPACE (or any
// configured key) to arm it.
struct MovementDebug {
    bool  arming      = false;   // gate is held and we may act
    bool  on_ground   = false;
    bool  crouching   = false;   // CTRL currently down
    float vz          = 0.0f;    // vertical velocity, units/sec
    float tti         = -1.0f;   // predicted ms to landing, -1 = unknown
    int   jumpbugs    = 0;       // crouch events fired this session
};

void Movement_Init();
void Movement_Shutdown();

MovementDebug Movement_GetDebug();

void Movement_SetJumpbug(bool on);
bool Movement_Jumpbug();
