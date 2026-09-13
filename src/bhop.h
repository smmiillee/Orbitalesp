// --- src/bhop.h ---
#pragma once
#include <cstdint>

// Bhop on HOLD-SPACE. Nothing fires otherwise -- there is no auto-jump, because
// an uncontrollable auto-jump is worse than no bhop at all.
//
// ══ WHAT THIS DOES AND DOES NOT WRITE ════════════════════════════════════
// INPUT MODE (default) performs ZERO writes to cs2.exe. It never touches the
// game's memory. It does synthesise keystrokes, and it does install a low-level
// keyboard hook -- but ReadProcessMemory/writing to the process is not involved.
//
// MEMORY MODE writes the jump button in cs2.exe (two int32 writes). It is
// offered only as a fallback, and it is the ONLY thing in this project that
// writes to the game.
//
// A bhop cannot be done purely read-only: reading memory cannot cause a jump.
//
// ══ WHY THE SPACE GATE USED TO BREAK IT ══════════════════════════════════
// While you hold space, the game already has +jump down, and Windows key
// auto-repeat keeps re-asserting your physical DOWN against any synthetic UP we
// send -- so the press edge at the landing tick frequently never forms. That is
// why timing was perfect without holding space and erratic with it.
//
// THE FIX: while bhop is driving, the keyboard hook SWALLOWS your physical
// spacebar, so the game sees only our synthetic events. That restores exactly
// the clean signal that gave perfect timing, and your held key becomes a gate
// rather than competing input.
//
// GROUND STATE is graded against the local player's Z (m_vOldOrigin, verified
// working):
//   z     - Z unchanged for ~22 ms -> standing. Works at any height, so a
//           landing off a roof reads exactly like a landing on flat ground.
//   flag  - once seen set while Z is static AND clear while Z moves it becomes
//           primary, being instance-accurate where the Z test needs a window.
//   hge   - same rule.
struct BhopDebug {
    uintptr_t offset  = 0;
    bool  on_ground   = false;
    bool  focused     = false;
    bool  space_held  = false;   // PHYSICAL spacebar state
    bool  hook_ok     = false;   // low-level keyboard hook installed
    bool  driving     = false;   // currently synthesising jump input
    int   signals     = 0;       // bit0 z, bit1 flag, bit2 hge
    bool  locked      = false;   // memory mode only: address confirmed
    bool  scanning    = false;
};

void Bhop_Init();       // starts the bhop / scanner / hook threads
void Bhop_Shutdown();   // stops and joins them, and releases any held key

BhopDebug Bhop_GetDebug();
void      Bhop_Rescan();     // memory mode only: forget the locked address

void Bhop_SetInputMode(bool enabled);
bool Bhop_InputMode();
