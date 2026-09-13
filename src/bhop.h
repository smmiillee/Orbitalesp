// --- src/bhop.h ---
#pragma once
#include <cstdint>

// Bhop writes the CS2 jump button:
//   +jump = 65537 (0x10001)   -jump = 256 (0x00000100)
//
// The ADDRESS is found at runtime by the scanner, so it never needs editing.
//
// GROUND DETECTION is what kept failing, and the reason is now known: the
// schema offset for m_fFlags is correct (0x3F4) but the client-side predicted
// pawn does not report FL_ONGROUND in bit 0, so the flag reads 0x10000 while
// you are standing still and bhop could never fire.
//
// So ground state is now taken from whichever signals PROVE themselves against
// the local player's Z position (m_vOldOrigin, which is verified working):
//
//   z     - Z unchanged for ~22 ms -> standing on something. Always available,
//           works on any surface and at any height (a landing off a roof looks
//           exactly like a landing on flat ground).
//   flag  - used only after it has been seen both set while Z is static and
//           clear while Z is moving.
//   hge   - m_hGroundEntity, used only after the same proof.
//
// A signal that never proves itself is simply ignored, so a wrong offset can
// degrade the result but can't break it.
struct BhopDebug {
    uintptr_t offset      = 0;
    bool      on_ground   = false;
    int       signals     = 0;   // bit0 z, bit1 flag, bit2 hge
    int       calibrating = 0;   // 0..100
    bool      locked      = false;
    bool      scanning    = false;
};

void Bhop_Init();
void Bhop_Shutdown();

BhopDebug Bhop_GetDebug();
void      Bhop_Rescan();

void Bhop_SetInputMode(bool enabled);
bool Bhop_InputMode();
