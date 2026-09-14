// --- src/bhop.h ---
#pragma once
#include <cstdint>

// Six INDEPENDENT bhop engines. Each is a complete feature with its own enable
// flag, its own state and its own output. Disabling one cannot affect another.
//
// NOTHING HERE WRITES TO cs2.exe. Every engine injects input (or types a console
// command); the project stays read-only towards the game process.
//
// ══ WHY THE PREVIOUS VERSION KEPT STOPPING ══════════════════════════════
// The key engines were EDGE-TRIGGERED: they injected once on the rising edge of
// the ground flag. If a hop was missed, you ended up standing on the ground with
// the flag stuck true -- so no new rising edge ever occurred and the engine
// NEVER INJECTED AGAIN until the gate was released and re-pressed. One miss
// killed the whole chain, which is exactly the "stops randomly" symptom.
//
// Every key engine now RETRIES while grounded: it keeps injecting at a fixed
// cadence for as long as the flag says you are on the ground. A missed hop is
// therefore self-correcting -- the next attempt happens a tick later.
//
// ══ THE ENGINES ═════════════════════════════════════════════════════════
//
//  SCROLL_SI      Wheel spam via SendInput while the gate is held. Continuous,
//                 so it has no state to latch.
//
//  KEY_EDGE       Key pairs while grounded, first one immediately. The workhorse.
//
//  KEY_EDGE_DEL   Same, but the first pair of each grounded period waits one
//                 client tick (15.625 ms). Valve has changed when a landing jump
//                 is accepted more than once; this is the fix for builds where
//                 an immediate jump gets swallowed.
//
//  SCROLL_ME      Wheel spam via the older mouse_event API -- a different path
//                 through the Windows input stack than SendInput.
//
//  FPS64          Types "fps_max 64", then uses the KEY_EDGE_DEL pattern. At
//                 64 fps frames align 1:1 with the server tick, so the injected
//                 input lands in the tick intended. Disabling restores fps_max 0.
//
//  KEY_REPEAT     Ignores the ground flag ENTIRELY. Injects a pair at a fixed
//                 cadence for as long as the gate is held. This is the most
//                 latch-proof engine possible: there is no state machine to get
//                 stuck, and while airborne the presses rely on jump buffering.
//
// ══ THE SPACE GATE ══════════════════════════════════════════════════════
// Holding space is the GATE, never input we rely on: a held +jump cannot make a
// new press edge. While any engine is running the hook swallows your physical
// spacebar so the game only ever sees our edges.
enum BhopEngine : int {
    BHOP_SCROLL_SI = 0,
    BHOP_KEY_EDGE,
    BHOP_KEY_EDGE_DEL,
    BHOP_SCROLL_ME,
    BHOP_FPS64,
    BHOP_KEY_REPEAT,
    BHOP_ENGINE_COUNT
};

struct BhopDebug {
    // shared
    bool  focused     = false;
    bool  space_held  = false;
    bool  hook_ok     = false;
    bool  on_ground   = false;
    bool  suppressing = false;
    int   signals     = 0;      // bit0 z, bit1 flag, bit2 hge

    // per-engine
    bool  active[BHOP_ENGINE_COUNT]   = {};
    int   injected[BHOP_ENGINE_COUNT] = {};
    // ms since that engine last injected; -1 if it never has. This is the
    // diagnostic that separates "our logic stalled" from "the game ignored us".
    int   age_ms[BHOP_ENGINE_COUNT]   = {};

    // FPS64
    bool  fps_cmd_sent = false;
    int   fps_target   = 64;
};

void Bhop_Init();
void Bhop_Shutdown();

BhopDebug Bhop_GetDebug();

void Bhop_SetEngine(int engine, bool on);
bool Bhop_GetEngine(int engine);

// Per-engine tuning.
void  Bhop_SetScrollInterval(float ms);   // SCROLL_SI / SCROLL_ME
float Bhop_ScrollInterval();
void  Bhop_SetRepeatMs(float ms);         // retry cadence for the key engines
float Bhop_RepeatMs();
void  Bhop_SetDelayMs(float ms);          // KEY_EDGE_DEL / FPS64 first-pair delay
float Bhop_DelayMs();
void  Bhop_SetInjectKey(int vk);
int   Bhop_InjectKey();
void  Bhop_SetFpsTarget(int fps);
int   Bhop_FpsTarget();
