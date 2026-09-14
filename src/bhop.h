// --- src/bhop.h ---
#pragma once
#include <cstdint>

// One bhop engine. SPACE only, no keybind needed, no memory writes.
//
// ══ WHY EVERY PREVIOUS ATTEMPT MISSED ═══════════════════════════════════
// The injected key was sent as a down+up PAIR inside a SINGLE SendInput call.
// That makes the key transition down and back up in essentially zero
// microseconds. CS2 samples OS keyboard state ONCE PER FRAME (subtick
// timestamps are computed by the client from that sampling). A zero-width press
// is therefore very likely never observed by any frame sample at all --
// it is not a mistimed input, it is an INVISIBLE one.
//
// That single bug explains the whole pattern: the scroll engines (discrete
// events, presumably queued) worked better than the key engines, and the key
// engines only ever worked occasionally -- whenever a zero-width press happened
// to fall across a frame boundary.
//
// THE FIX: hold the key DOWN for long enough to span at least one frame, then
// release it, then retry. The hold is the mechanism; `hold_ms` is the control
// that matters, sized against frame time rather than tick time.
//
// ══ TICK vs FRAME (correcting an earlier mistake) ═══════════════════════
// Subtick sits ON TOP of the tick -- the server still ticks (64 in MM, 128
// elsewhere). Subtick means input carries a sub-tick timestamp which the server
// resolves at that fractional position.
//
// The consequence for us is that the CLIENT decides when our input happened,
// from its per-frame read of OS keyboard state. So the limiting clock is FRAME
// time, not tick time. An earlier version aligned presses to tick boundaries,
// which was modelling the wrong clock.
enum { BHOP_ENGINE_COUNT = 1 };

struct BhopDebug {
    bool  focused     = false;
    bool  space_held  = false;
    bool  hook_ok     = false;
    bool  on_ground   = false;
    bool  suppressing = false;
    bool  pressing    = false;   // key currently held down
    int   signals     = 0;       // bit0 z, bit1 flag, bit2 hge

    int   injected    = 0;       // press events started this session
    int   age_ms      = -1;      // ms since the last press, -1 = never
    float hold_ms     = 0.0f;    // how long the key is held per press
    float retry_ms    = 0.0f;    // interval between presses while grounded
};

void Bhop_Init();
void Bhop_Shutdown();

BhopDebug Bhop_GetDebug();

void Bhop_SetEnabled(bool on);
bool Bhop_Enabled();

// How long the key stays DOWN per press. Must exceed one frame or the press is
// invisible to the game's input sampling. 144 fps -> 6.9 ms/frame.
void  Bhop_SetHoldMs(float ms);
float Bhop_HoldMs();

// Interval between the START of successive presses while grounded. Retrying is
// what stops a missed hop from ending the chain.
void  Bhop_SetRetryMs(float ms);
float Bhop_RetryMs();
