// --- src/bhop.h ---
#pragma once
#include <cstdint>

// Bhop by KEYSTROKE INJECTION ONLY. Nothing in this file writes to cs2.exe.
//
// Removing the memory-write bhop means the whole project now performs ZERO
// writes to the game process -- it only reads memory and injects keystrokes.
//
// ══ HOW HOLD-SPACE WORKS ═════════════════════════════════════════════════
// Holding the physical key is a GATE, not the input. While bhop is driving, the
// keyboard hook swallows your physical spacebar, because holding it makes the
// engine's +jump stay down -- and a jump needs a fresh PRESS edge, so a held key
// means you can never re-jump on landing. With your key swallowed, the game's
// jump input comes only from us and every landing gets its own clean edge.
//
// ══ THE TWO OUTPUT MODES ═════════════════════════════════════════════════
//  PLAIN      - press when the ground flag says you are grounded. Simple, and
//               correct as far as it goes, but the flag is only written once
//               per game tick (~15.6 ms), so by the time we see it the landing
//               tick may already have passed.
//
//  PREDICTIVE - instead of waiting to OBSERVE the landing, estimate when it
//               will happen from your vertical velocity and press slightly
//               early, so the press is already down when the engine samples
//               input for the landing tick. The observed ground flag is still
//               used as a fallback, so prediction can only help.
//
// ══ HONEST STATUS ════════════════════════════════════════════════════════
// Prediction is an EXPERIMENT. There is no reference implementation to copy --
// the cs2-bhop repo people cite for this is itself a memory-write bhop, and the
// SendInput snippet circulating online is not from it. Whether early-pressing
// lands inside the right input window depends on engine internals I cannot
// inspect, so treat this as something to measure rather than something proven.
struct BhopDebug {
    // config
    bool enabled      = true;
    bool predict      = false;
    bool separate_key = false;

    // live state
    bool  on_ground   = false;
    bool  focused     = false;
    bool  space_held  = false;   // PHYSICAL spacebar state
    bool  hook_ok     = false;   // low-level keyboard hook installed
    bool  driving     = false;   // currently commanding the jump
    bool  suppressing = false;   // physical spacebar is being swallowed

    int   signals     = 0;       // bit0 z, bit1 flag, bit2 hge
    int   edges       = 0;       // press edges generated
    int   predicted   = 0;       // of those, how many prediction armed

    float vz          = 0.0f;    // vertical velocity, units/sec
    float tti         = -1.0f;   // ms to predicted impact, -1 if unknown
};

void Bhop_Init();
void Bhop_Shutdown();

BhopDebug Bhop_GetDebug();

void  Bhop_SetEnabled(bool on);
void  Bhop_SetPredict(bool on);
void  Bhop_SetSeparateKey(bool on);
void  Bhop_SetLead(float ms);   // prediction lead, ms
float Bhop_Lead();
