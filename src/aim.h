// --- src/aim.h ---
#pragma once
#include <cstdint>

// Triggerbot.
//
// DETECTION is read-only. FIRING injects a click -- the game only shoots when
// it receives one, so this cannot be made read-only.
//
// ══ VISIBILITY: WHAT IS AND IS NOT POSSIBLE EXTERNALLY ══════════════════
// A real line-of-sight check needs a raycast against map geometry, which an
// external cannot do without parsing the map VPK. The standard workaround is
// m_bSpottedByMask, but that is RADAR state: it reflects teammates' spotting and
// audio, not whether YOU can see the target.
//
// So the vis check here is APPROXIMATE and SELF-DISABLING: if the mask never
// reads nonzero it concludes the offset is wrong for this build and switches
// itself off, rather than silently blocking every shot. The UI says which.
//
// SMOKE is not detectable externally and is not implemented.
// FLASH needs an offset that is not verified for this build, so it is not
// implemented either -- better absent than blocking every shot on a bad read.
//
// ══ AIM RADIUS ══════════════════════════════════════════════════════════
// Hardcoded per bone, scaled by resolution. No user adjustment.
struct AimDebug {
    bool  enabled     = false;
    bool  firing      = false;
    bool  on_target   = false;
    bool  has_vis     = false;   // the target passed the vis check
    bool  vis_usable  = false;   // the vis check has proved itself this session
    int   vis_samples = 0;       // how many masks were read
    int   vis_hits    = 0;       // how many were nonzero
    const char* blocked_by = "-"; // "team" / "vis" / "-"
    float target_dist = 0.0f;
    const char* target_bone = "-";
    int   delay_ms    = 0;
    int   held_ms     = 0;
};

void Aim_Init();
void Aim_Shutdown();

AimDebug Aim_GetDebug();

void Aim_SetEnabled(bool on);
bool Aim_Enabled();

void Aim_SetFire(bool on);
bool Aim_Fire();

// 0 means UNBOUND, which means the trigger does not run at all.
void Aim_SetKey(int vk);
int  Aim_Key();

// Do not fire at teammates.
void Aim_SetTeamCheck(bool on);
bool Aim_TeamCheck();

// Approximate spotted-based visibility check. Self-disabling.
void Aim_SetVisCheck(bool on);
bool Aim_VisCheck();

// Acquisition delay, 0-600 ms.
void Aim_SetDelay(int ms);
int  Aim_Delay();
