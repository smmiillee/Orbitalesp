// --- src/bhop.h ---
#pragma once
#include <cstdint>

// Bhop writes the CS2 jump button:
//   +jump = 65537 (0x10001)   -jump = 256 (0x00000100)
// Only the ADDRESS moves, and the scanner finds it at runtime.
//
// GROUND DETECTION is what kept failing. The schema offset for m_fFlags is
// correct (0x3F4), but the client-side predicted pawn does not report
// FL_ONGROUND in bit 0 -- it reads 0x10000 (bit 16) while you are standing
// still, so a flag-only test could never fire.
//
// Ground state therefore comes from signals that PROVE THEMSELVES against the
// local player's Z position (m_vOldOrigin, verified working):
//
//   z     - Z unchanged for ~22 ms -> standing. Always available, and it is
//           height- and surface-independent: a landing off a roof reads
//           exactly like a landing on flat ground.
//   flag  - used only after it has actually been seen set while Z is static
//           AND clear while Z is moving.
//   hge   - same rule.
//
// A signal that never proves itself is ignored, so a wrong offset degrades the
// result instead of breaking it.
struct BhopDebug {
    uintptr_t offset      = 0;
    bool      on_ground   = false;
    bool      focused     = false;  // is CS2 actually the foreground window?
    int       signals     = 0;      // bit0 z, bit1 flag, bit2 hge
    int       calibrating = 0;      // 0..100
    bool      locked      = false;
    bool      scanning    = false;
};

void Bhop_Init();
void Bhop_Shutdown();

BhopDebug Bhop_GetDebug();
void      Bhop_Rescan();

void Bhop_SetInputMode(bool enabled);
bool Bhop_InputMode();
