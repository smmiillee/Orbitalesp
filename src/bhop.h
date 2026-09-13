// --- src/bhop.h ---
#pragma once
#include <cstdint>

// Bhop writes the CS2 jump button:
//   +jump = 65537 (0x10001)   -jump = 256 (0x00000100)
// Both have been stable for the life of CS2. Only the ADDRESS moves.
//
// GROUND DETECTION is the part that kept breaking. m_fFlags is a per-update
// offset, and a wrong offset doesn't fail loudly -- it just reads a constant
// (we were reading bit 16, FL_AIMTARGET, which is why ground: never said YES).
//
// So the ground flag is now CALIBRATED AT RUNTIME against the local player's
// Z position, which is known-good (the ESP is aligned, so m_vOldOrigin is
// definitely right):
//   * Z unchanged between samples  -> the player is standing on something
//   * Z rising fast                -> they just jumped
//   * Z dropping fast              -> they are falling
// The reader watches a window of u32s in the pawn and keeps whichever offset
// actually splits into "set while standing, clear while airborne".
struct BhopDebug {
    uintptr_t offset        = 0;      // current jump-button offset
    uint32_t  live_value    = 0;      // live dword at that address
    bool      on_ground     = false;  // combined ground verdict
    uint32_t  flags         = 0;      // value at the calibrated flag offset
    uintptr_t flag_offset   = 0;      // calibrated m_fFlags offset (0 = none)
    int       calib_score   = 0;      // confidence of the calibration
    bool      locked        = false;  // jump address confirmed by the scanner
    bool      scanning      = false;  // sweep in progress
};

void Bhop_Init();      // starts the bhop + scanner threads (after attach)
void Bhop_Shutdown();  // stops and joins them

BhopDebug Bhop_GetDebug();
void      Bhop_Rescan();  // forget the locked jump address and sweep again

void Bhop_SetInputMode(bool enabled);
bool Bhop_InputMode();
</｜DSML｜ parameter>
</｜DSML｜ invoke>
</｜DSML｜ calls>
