// --- src/bhop.h ---
#pragma once
#include <cstdint>

// Bhop has two backends.
//
//  * INPUT mode (default) -- synthesises the spacebar with SendInput, holding it
//    while you are grounded and releasing it in the air. That is exactly the
//    macro a bhop script performs, and it needs NO memory offsets at all, so a
//    game update cannot break it. Auto-jumps while the Bhop checkbox is on.
//
//  * MEMORY mode -- writes the CS2 jump button (+jump 65537 / -jump 256). The
//    address moves on every update and cannot be derived, so it is scanned for
//    and then CONFIRMED against a key you are actually holding. Nothing locks
//    from a guess.
//
// GROUND STATE is graded against the local player's Z (m_vOldOrigin, verified):
//   z     - Z unchanged for ~22 ms -> standing. Works on any surface at any
//           height: a landing off a roof reads exactly like a landing on flat.
//   flag  - once it has been seen set while Z is static AND clear while Z moves,
//           it becomes the primary signal because it is instance-accurate
//           rather than a 22 ms window.
//   hge   - same rule.
struct BhopDebug {
    uintptr_t offset  = 0;
    bool  on_ground   = false;
    bool  focused     = false;
    int   signals     = 0;   // bit0 z, bit1 flag, bit2 hge
    bool  locked      = false;
    bool  scanning    = false;
    int   candidates  = 0;
};

void Bhop_Init();
void Bhop_Shutdown();

BhopDebug Bhop_GetDebug();
void      Bhop_Rescan();

void Bhop_SetInputMode(bool enabled);
bool Bhop_InputMode();
