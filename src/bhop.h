// --- src/bhop.h ---
#pragma once
#include <cstdint>

// One bhop engine. SPACE is the only injected key -- no bind needed, because
// `space` is already bound to +jump by default.
//
// NOTHING HERE WRITES TO cs2.exe. The engine injects keystrokes; the project
// stays read-only towards the game process.
//
// ══ HOW IT WORKS ═════════════════════════════════════════════════════════
// Holding space is the GATE. While the engine runs, the hook swallows your
// physical spacebar, so the game's +jump input comes only from us -- your held
// key can never block the press edges we need.
//
// A jump needs a fresh PRESS EDGE while grounded, so the engine injects a
// SPACE down+up pair, and retries while the ground flag stays true. Retrying is
// what stops a missed hop from ending the chain: the flag sticking true no
// longer silences the engine.
//
// ══ THE TIMING FIX: PRESS ON A TICK BOUNDARY ════════════════════════════
// The retry cadence used to be "15.625 ms from when we noticed". But detection
// happens somewhere INSIDE a tick (up to one sample late), so every retry was
// offset by a random phase and the presses drifted around the tick -- which is
// why it stopped hoppjing cleanly.
//
// The fix is a tick clock. Ground transitions can only happen on tick
// boundaries, so we watch them, estimate the tick period and phase, and then
// aim each press AT a boundary instead of at an interval. The offset control
// shifts where inside the tick the press lands, so you can dial it in.
//
// ══ GROUND STATE ════════════════════════════════════════════════════════
// m_fFlags bit 0, tested as a BITMASK and graded against Z before being
// trusted. Z (m_vOldOrigin) is the reference because it is verified working.
struct BhopDebug {
    bool  focused      = false;
    bool  space_held   = false;
    bool  hook_ok      = false;
    bool  on_ground    = false;
    bool  suppressing  = false;
    int   signals      = 0;      // bit0 z, bit1 flag, bit2 hge

    int   injected     = 0;      // pairs injected this session
    int   age_ms       = -1;     // ms since the last pair, -1 = never

    // tick clock
    bool  locked       = false;  // clock has converged
    float tick_ms      = 0.0f;   // estimated tick period
    float last_phase   = -1.0f;  // phase of the last injection, ms into tick
};

void Bhop_Init();
void Bhop_Shutdown();

BhopDebug Bhop_GetDebug();

void Bhop_SetEnabled(bool on);
bool Bhop_Enabled();

// Press immediately on landing detection, or aim at the next tick boundary.
void  Bhop_SetTickLock(bool on);
bool  Bhop_TickLock();

// Where inside the tick to press (tick-lock only). 0 = on the boundary.
void  Bhop_SetOffsetMs(float ms);
float Bhop_OffsetMs();

// How many ticks between retries while grounded. 1 = every tick.
void  Bhop_SetRetryTicks(int ticks);
int   Bhop_RetryTicks();
