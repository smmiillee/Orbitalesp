// --- src/aim.h ---
#pragma once
#include <cstdint>

// Triggerbot.
//
// DETECTION is read-only. FIRING injects a click -- the game only shoots when it
// receives one, so this cannot be read-only.
//
// All three behaviours are HARDCODED ON rather than exposed as toggles:
//   team check  ON  never shoot teammates
//   vis check   ON  approximate, see below
//   firing      ON
//
// ══ VISIBILITY IS APPROXIMATE ══════════════════════════════════════════
// m_bSpottedByMask is RADAR state, not line of sight -- it reflects whether
// anyone on your team has spotted the target. It therefore lets some wall shots
// through. A true check needs a raycast, which an external cannot do without
// parsing map geometry.
//
// It SELF-DISABLES if the mask never reads nonzero, because that means the
// offset is wrong for this build. Better to stop blocking than to silently
// refuse every shot.
//
// Hit detection uses the whole bone WIREFRAME (segment distance), not just the
// joints, so the crosshair does not have to land exactly on a bone.
struct AimDebug {
    bool  enabled     = false;
    bool  firing      = false;
    bool  on_target   = false;
    bool  has_vis     = false;    // target passed the vis check
    bool  vis_usable  = false;    // the vis check proved itself this session
    int   vis_samples = 0;
    int   vis_hits    = 0;
    const char* blocked_by = "-"; // "team" / "vis" / "-"
    const char* target_bone = "-";
    float target_dist = 0.0f;
    int   weapon_id   = 0;        // local active weapon def index, 0 if unknown
};

void Aim_Init();
void Aim_Shutdown();

AimDebug Aim_GetDebug();

void Aim_SetEnabled(bool on);
bool Aim_Enabled();

// 0 means UNBOUND, which means the trigger does not run at all.
void Aim_SetKey(int vk);
int  Aim_Key();
