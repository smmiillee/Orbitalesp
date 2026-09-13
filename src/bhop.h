// --- src/bhop.h ---
#pragma once
#include <cstdint>

// Bhop works on HOLD-SPACE only. Nothing fires otherwise -- there is no
// auto-jump, because an uncontrollable auto-jump is worse than no bhop at all.
//
// HOW THE SPACE GATE WORKS: input mode injects keypresses, and SendInput feeds
// back into GetAsyncKeyState, so a plain state check latches itself on and can
// never see your release. The gate therefore reads PHYSICAL key state through a
// low-level keyboard hook, which tags injected events with LLKHF_INJECTED and
// lets them be ignored. See kb_proc() in bhop.cpp.
//
//  * INPUT mode (default) -- injects a release/press pair around each landing.
//    It needs NO memory offsets, so a game update cannot break it. While you
//    hold space the game already has +jump down, so the injected pair is what
//    creates the press EDGE at the landing tick -- which is precisely the macro
//    a bhop script performs.
//
//  * MEMORY mode -- writes the CS2 jump button directly (+jump 65537 /
//    -jump 256). The address moves on every update and cannot be derived, so it
//    is scanned for and then CONFIRMED against a key you are actually holding.
//    Nothing locks from a guess.
//
// GROUND STATE is graded against the local player's Z (m_vOldOrigin, verified):
//   z     - Z unchanged for ~22 ms -> standing. Works at any height, so a
//           landing off a roof reads exactly like a landing on flat ground.
//   flag  - once seen set while Z is static AND clear while Z moves, it becomes
//           the primary signal, because it is instance-accurate where the Z test
//           needs a 22 ms window. That matters when the grounded frame is a
//           single tick.
//   hge   - same rule.
struct BhopDebug {
    uintptr_t offset   = 0;
    bool  on_ground    = false;
    bool  focused      = false;
    bool  space_held   = false;  // PHYSICAL spacebar state
    bool  hook_ok      = false;  // low-level keyboard hook installed
    int   signals      = 0;      // bit0 z, bit1 flag, bit2 hge
    bool  locked       = false;
    bool  scanning     = false;
};

void Bhop_Init();
void Bhop_Shutdown();

BhopDebug Bhop_GetDebug();
void      Bhop_Rescan();

void Bhop_SetInputMode(bool enabled);
bool Bhop_InputMode();
