// --- src/bhop.h ---
#pragma once
#include <cstdint>

// Bhop on HOLD-SPACE. Nothing fires otherwise -- there is no auto-jump.
//
// ══ WHAT GETS WRITTEN ════════════════════════════════════════════════════
// INPUT MODE (default) performs ZERO writes to cs2.exe. It synthesises
// keystrokes and installs a low-level keyboard hook, but it never writes to the
// game's memory.
//
// MEMORY MODE writes the jump button in cs2.exe (two int32 writes). It is the
// only thing in this project that writes to the game, and it is the only way to
// get tick-synchronous jump timing. A bhop cannot be done purely read-only:
// reading memory cannot cause a jump.
//
// ══ WHY HOLD-SPACE BROKE THINGS ══════════════════════════════════════════
// With the key held, the game already has +jump down and Windows key
// auto-repeat keeps re-asserting your physical DOWN against any synthetic UP,
// so the press edge at the landing often never formed.
//
// THE FIX: while bhop is driving, the hook SWALLOWS your physical spacebar, so
// the game's jump input comes only from us. Your held key becomes a gate rather
// than competing input.
//
// ══ WHY INPUT MODE USED TO BREAK CHAINS ══════════════════════════════════
// The key used to be held for the whole grounded period and released only in
// the air. A missed landing therefore left the key STUCK DOWN, and the next
// landing had no fresh press edge -- so the chain could never recover. A single
// flicker of the ground flag mid-air caused the same failure.
//
// Now each landing produces a press, and the key is always released again a
// tick later, so every landing gets its own edge and a miss can retry.
//
// GROUND STATE is graded against the local player's Z (m_vOldOrigin, verified):
//   z     - Z unchanged for ~22 ms -> standing. Works at any height, so a
//           landing off a roof reads exactly like a landing on flat ground.
//   flag  - once seen set while Z is static AND clear while Z moves it becomes
//           primary, being instance-accurate where the Z test needs a window.
//   hge   - same rule.
struct BhopDebug {
    uintptr_t offset   = 0;
    bool  on_ground    = false;
    bool  focused      = false;
    bool  space_held   = false;  // PHYSICAL spacebar state
    bool  hook_ok      = false;  // low-level keyboard hook installed
    bool  driving      = false;  // currently synthesising jump input
    int   signals      = 0;      // bit0 z, bit1 flag, bit2 hge
    int   presses      = 0;      // press edges generated this session
    bool  locked       = false;  // memory mode only: address confirmed
    bool  scanning     = false;
};

void Bhop_Init();
void Bhop_Shutdown();

BhopDebug Bhop_GetDebug();
void      Bhop_Rescan();     // memory mode only: forget the locked address

void Bhop_SetInputMode(bool enabled);
bool Bhop_InputMode();
