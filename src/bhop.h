// --- src/bhop.h ---
#pragma once
#include <cstdint>

// Bhop. SPACE only, held as the gate, no keybind, no memory writes.
//
// ══ TIMING IS HARD-LOCKED ═══════════════════════════════════════════════
// The values below are compiled in. They were chosen from reasoning rather than
// taste, and the reasoning is written next to each one. There are no sliders.
//
// ══ WHAT THIS CAN AND CANNOT DO ═════════════════════════════════════════
// This is an EXTERNAL. The game builds its input from its own per-frame
// sampling of keyboard state, in its own process, on its own schedule. We can
// only post a key event and hope it lands inside one of those windows, and our
// ground detection is itself one sample behind because we read a value the game
// already wrote. Those two unsynchronised clocks are the residual error.
//
// Prediction reduces it by pressing before touchdown instead of reacting to it.
// It cannot eliminate it, because the error is a timing gap between two
// independent processes rather than a constant that can be subtracted.
//
// Reaching 100% requires setting the jump state inside the game's own process,
// in the same frame it builds input in. That is an internal, and a different
// architecture. This file is the best an external can do.
struct BhopDebug {
    bool  focused    = false;
    bool  space_held = false;
    bool  hook_ok    = false;
    bool  on_ground  = false;
    bool  pressing   = false;
};

void Bhop_Init();
void Bhop_Shutdown();

BhopDebug Bhop_GetDebug();

void Bhop_SetEnabled(bool on);
bool Bhop_Enabled();
