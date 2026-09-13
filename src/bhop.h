// --- src/bhop.h ---
#pragma once
#include <cstdint>

// Bhop writes the CS2 jump button:
//   +jump = 65537 (0x10001)   -jump = 256 (0x00000100)
//
// The ADDRESS is found at runtime, so it never needs editing:
//
//  * method 1 (no keys needed) - sweep for the 13-slot button block: 13 dwords
//    on the 0x90 stride that are ALL exactly 0 or 256. That combination is
//    essentially unique, and it works while you are standing still.
//  * method 2 - while SPACE is held, find the dword holding 0x10001 and check
//    the same block structure. Unambiguous, and it can't lock onto `forward`.
//
// GROUND DETECTION is graded against the local player's Z position
// (m_vOldOrigin, verified working), because the client-side predicted pawn does
// NOT set FL_ONGROUND in bit 0 -- it reads 0x10000 while standing still, which
// is why a flag-only test could never fire.
//
//   z     - Z unchanged for ~22 ms -> standing. Works on any surface at any
//           height: a landing off a roof reads exactly like a landing on flat.
//   flag  - used only after it has been seen set while Z is static AND clear
//           while Z is moving.
//   hge   - same rule.
//
// A signal that never proves itself is ignored, so a wrong offset degrades the
// result instead of breaking it.
struct BhopDebug {
    uintptr_t offset      = 0;
    bool      on_ground   = false;
    bool      focused     = false;
    int       signals     = 0;      // bit0 z, bit1 flag, bit2 hge
    bool      locked      = false;
    bool      scanning    = false;
};

void Bhop_Init();
void Bhop_Shutdown();

BhopDebug Bhop_GetDebug();
void      Bhop_Rescan();

void Bhop_SetInputMode(bool enabled);
bool Bhop_InputMode();
