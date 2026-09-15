// --- src/aim.h ---
#pragma once
#include <cstdint>

// Triggerbot.
//
// DETECTION is read-only. FIRING injects a click -- the game only shoots when it
// receives one, so this cannot be read-only.
//
// team check and firing are hardcoded ON.
//
// ══ VIS CHECK: SOFT, NOT EXACT ═════════════════════════════════════════
// m_bSpottedByMask is RADAR state -- "has anyone on my team spotted this enemy
// recently" -- not line of sight. It both persists after someone breaks line of
// sight (so a hard gate shoots walls) and lags for you (so a hard gate refuses
// tight angles you can actually see).
//
// So it is used as a SOFT gate: it only blocks once the mask has read zero
// continuously, which means a brief appearance is never blocked. It cannot be
// made exact externally; a true check needs a raycast against map geometry.
//
// ══ HIT TEST ═══════════════════════════════════════════════════════════
// Crosshair vs. the whole bone WIREFRAME (segment distance, main links plus
// extra shoulder/spine/hand links), with a world-space radius converted to
// pixels per target so the margin scales with distance instead of being a fixed
// pixel count.
struct AimDebug {
    bool  enabled     = false;
    bool  firing      = false;
    bool  on_target   = false;
    bool  vis_usable  = false;   // offset has read nonzero at least once
    bool  vis_blocked = false;   // blocked by sustained occlusion this frame
    int   vis_samples = 0;
    int   vis_hits    = 0;
    const char* target_hit = "-"; // "head" / "body"
    float target_dist = 0.0f;
    float mesh_radius = -1.0f;    // pixels, at the winning target
    int   weapon_id   = 0;
};

void Aim_Init();
void Aim_Shutdown();

AimDebug Aim_GetDebug();

void Aim_SetEnabled(bool on);
bool Aim_Enabled();

// 0 means UNBOUND, which means the trigger does not run at all.
void Aim_SetKey(int vk);
int  Aim_Key();
