// --- src/bhop.h ---
#pragma once
#include <cstdint>

// One bhop engine. SPACE only, no keybind, no memory writes.
//
// ══ THE TWO MECHANISMS ══════════════════════════════════════════════════
//
// 1. HOLD. An injected down+up pair inside a single SendInput call leaves the
//    key down for ~0 us, and CS2 samples keyboard state once per frame -- so
//    such a press is frequently never observed. The key must therefore be held
//    long enough to span a frame. This is already fixed and working.
//
// 2. PRESS BEFORE THE LANDING. This is the remaining miss. We currently press
//    AFTER observing the ground flag, which means the press is always late
//    relative to the landing sample. A human scrolling continuously gets a
//    notch in flight before touchdown, which is why manual bhop feels better.
//
//    So we estimate the landing from vertical velocity and start the press
//    `lead_ms` BEFORE it, holding across the predicted touchdown. The key is
//    then already down when the landing is sampled.
//
//    `lead_ms` is the control that matters now.
//
// The ground-flag retry is kept as a fallback, so if prediction is wrong we
// still press once we can see we are grounded. Prediction can only add presses.
//
// ══ HONEST LIMIT ════════════════════════════════════════════════════════
// Our ground detection is asynchronous to the game's input sampling, so the
// gap between detection and the sampled tick cannot be closed from outside the
// process. Prediction closes most of it by acting early instead of reacting.
// It cannot be proven error-free from here.
struct BhopDebug {
    bool  focused     = false;
    bool  space_held  = false;
    bool  hook_ok     = false;
    bool  on_ground   = false;
    bool  suppressing = false;
    bool  pressing    = false;   // key currently down

    int   injected    = 0;       // presses started this session
    int   age_ms      = -1;      // ms since last press, -1 = never

    float vz          = 0.0f;    // vertical velocity, units/sec
    float tti         = -1.0f;   // predicted ms to landing, -1 = unknown
    int   pred_hits   = 0;       // presses started by prediction
};

void Bhop_Init();
void Bhop_Shutdown();

BhopDebug Bhop_GetDebug();

void Bhop_SetEnabled(bool on);
bool Bhop_Enabled();

// How early to press before the predicted landing, in ms.
void  Bhop_SetLeadMs(float ms);
float Bhop_LeadMs();

// How long the key stays down per press. Must exceed one frame.
void  Bhop_SetHoldMs(float ms);
float Bhop_HoldMs();

// Fallback retry interval while grounded.
void  Bhop_SetRetryMs(float ms);
float Bhop_RetryMs();
