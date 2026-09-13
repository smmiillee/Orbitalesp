// --- src/bhop.h ---
#pragma once
#include <cstdint>

// Bhop writes the CS2 jump button:
//   +jump = 65537 (0x10001)   -jump = 256 (0x00000100)
//
// THE ADDRESS is found at runtime and validated against your real key presses,
// because the button block moves independently of the client.dll globals and
// therefore cannot be derived from a dump of anything else.
//
// GROUND STATE comes from the signals that PROVE THEMSELVES against the local
// player's Z position (m_vOldOrigin, a verified offset):
//   z     - Z unchanged for ~22 ms -> standing. Always available, and
//           height-/surface-independent: a landing off a roof reads exactly
//           like a landing on the flat.
//   flag  - only used after it is seen set while Z is static AND clear while
//           Z moves.
//   hge   - same rule.
//
// INPUT MODE needs no offsets at all: it synthesises the spacebar, holding it
// down on the ground and releasing it in the air -- which is precisely what a
// bhop macro does. If memory mode ever stops working after an update, this
// still works.
struct BhopDebug {
    uintptr_t offset    = 0;
    bool  on_ground     = false;
    bool  focused       = false;
    int   signals       = 0;   // bit0 z, bit1 flag, bit2 hge
    bool  locked        = false;
    bool  scanning      = false;
    int   candidates    = 0;   // blocks that passed validation this sweep
};

void Bhop_Init();
void Bhop_Shutdown();

BhopDebug Bhop_GetDebug();
void      Bhop_Rescan();

void Bhop_SetInputMode(bool enabled);
bool Bhop_InputMode();
