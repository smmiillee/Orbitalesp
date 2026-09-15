// --- src/aim.h ---
#pragma once
#include <cstdint>

// Triggerbot.
//
// DETECTION is read-only. FIRING injects a click -- the game only shoots when it
// receives one, so this cannot be read-only.
//
// team check, vis check and firing are all hardcoded ON (see aim.cpp).
//
// ══ DELAY ══════════════════════════════════════════════════════════════
// Reaction time before the FIRST shot after acquiring a target, 0-600 ms.
// It does not gate sustained fire: once firing, the weapon's own cycle time
// paces the shots. At 0 the first shot happens on the next loop iteration.
//
// ══ VIS CHECK, SOFT ════════════════════════════════════════════════════
// m_bSpottedByMask is RADAR state, so it persists after a teammate loses sight
// (which is why wall shots still happen) and lags for you (which is why tight
// angles can be refused). It only blocks after sustained zeros.
//
// ══ HIT TEST ═══════════════════════════════════════════════════════════
// Crosshair vs. the whole bone wireframe, with a world-space radius converted
// per target so the margin scales with distance.
struct AimDebug {
    bool enabled = false;
    bool firing  = false;
};

void Aim_Init();
void Aim_Shutdown();

AimDebug Aim_GetDebug();

void Aim_SetEnabled(bool on);
bool Aim_Enabled();

// 0 means UNBOUND, which means the trigger does not run at all.
void Aim_SetKey(int vk);
int  Aim_Key();

// Reaction time before the first shot, 0-600 ms.
void Aim_SetDelay(int ms);
int  Aim_Delay();
