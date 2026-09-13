// --- src/bhop.h ---
#pragma once
#include <cstdint>

// Bhop on HOLD-SPACE. Nothing fires otherwise -- there is no auto-jump.
//
// ── WHY THIS USES MEMORY MODE, NOT INPUT INJECTION ───────────────────────
// Input injection (SendInput) cannot coexist with you holding the spacebar.
// While you hold it the game sees +jump continuously, and Windows' key
// auto-repeat keeps re-asserting the physical DOWN against any synthetic UP we
// send -- so the press edge at the landing tick frequently never forms.
//
// Writing the jump button in memory sidesteps all of that: it does not touch
// the keyboard, so it cannot fight your held key, and reading the physical
// spacebar with GetAsyncKeyState is reliable BECAUSE nothing is injecting.
// That combination is what makes "hold space = bhop" work.
//
//   +jump = 65537 (0x10001)   -jump = 256 (0x00000100)
//
// The ADDRESS moves on every update and cannot be derived from any dump (the
// button block and the globals live in different regions), so it is found at
// runtime and CONFIRMED against a key you are actually holding. Nothing locks
// from a guess.
//
// GROUND STATE is graded against the local player's Z (m_vOldOrigin, verified):
//   z     - Z unchanged for ~22 ms -> standing. Works at any height, so a
//           landing off a roof reads exactly like a landing on flat ground.
//   flag  - once seen set while Z is static AND clear while Z moves, it becomes
//           primary, because it is instance-accurate where the Z test needs a
//           22 ms window. That matters when the grounded frame is one tick.
//   hge   - same rule.
struct BhopDebug {
    uintptr_t offset  = 0;
    bool  on_ground   = false;
    bool  focused     = false;
    bool  space_held  = false;
    int   signals     = 0;      // bit0 z, bit1 flag, bit2 hge
    bool  locked      = false;
    bool  scanning    = false;
};

void Bhop_Init();
void Bhop_Shutdown();

BhopDebug Bhop_GetDebug();
void      Bhop_Rescan();

void Bhop_SetInputMode(bool enabled);
bool Bhop_InputMode();
</｜DSML｜ parameter>

</｜DSML｜ calls>
