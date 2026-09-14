// --- src/bhop.h ---
#pragma once
#include <cstdint>

// Five INDEPENDENT bhop engines. Each one is a complete feature: it has its own
// enable flag, its own input method and its own state. Disabling one does not
// affect any other, and they can be combined (they will simply both inject).
//
// NOTHING HERE WRITES TO cs2.exe. Every engine either injects keystrokes/mouse
// events or, in the 64 FPS case, types a console command. The project remains
// read-only towards the game process.
//
// ── THE ENGINES, AND WHY EACH EXISTS ────────────────────────────────────
//
//  SCROLL_SI   Spams mouse-wheel events via SendInput while the gate is held.
//              Timing-agnostic: several events per tick, so one lands in the
//              window. The community-standard technique.
//
//  KEY_SI      Injects one F20 down+up PAIR on each observed landing. This is
//              the arrangement reported as working on UnknownCheats: a lot of
//              VK codes are ignored by CS2, but F13-F24 are unused by the game
//              and are accepted. Requires an in-game bind (see MISC tab).
//
//  KEY_SI_DEL   Same as KEY_SI but waits one client tick (15.625 ms) after the
//              ground flag appears before injecting. Valve changed when a
//              landing jump is accepted more than once, and a one-tick delay is
//              the fix people used for the versions that needed it. Worth
//              testing because we cannot tell which regime this build is in.
//
//  SCROLL_ME   Wheel spam through the OLDER mouse_event API. It travels a
//              different path through the Windows input stack than SendInput,
//              so if CS2 filters one it may not filter the other.
//
//  FPS64       Sends "fps_max 64" to the CS2 console, then injects tick-aligned
//              key pairs. At 64 fps frames line up 1:1 with the server tick, so
//              the injected input lands in the tick intended rather than being
//              quantised. Disabling it restores fps_max 0.
//
// ── THE SPACE GATE ──────────────────────────────────────────────────────
// Holding space is the GATE; it is never the input we rely on. A held +jump
// cannot produce a new press edge, so while any engine is running the hook
// swallows your physical spacebar and the edges come from us.
//
// ── GROUND STATE ────────────────────────────────────────────────────────
// m_fFlags bit 0 tested as a BITMASK, never as a whole-value comparison -- the
// UnknownCheats thread reports the bitmask form being materially more
// consistent, and our own grading has already confirmed bit 0 is meaningful on
// this build (the MISC tab showed "z flag hge" as trusted signals).
enum BhopEngine : int {
    BHOP_SCROLL_SI = 0,
    BHOP_KEY_SI,
    BHOP_KEY_SI_DEL,
    BHOP_SCROLL_ME,
    BHOP_FPS64,
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

    // per-engine: 1 while that engine is active and driving
    bool  active[BHOP_ENGINE_COUNT]  = {};
    // per-engine: inputs injected this session (wheels, pairs or commands)
    int   injected[BHOP_ENGINE_COUNT] = {};
    // FPS64 only
    bool  fps_cmd_sent = false;
    int   fps_target   = 64;
};

void Bhop_Init();
void Bhop_Shutdown();

BhopDebug Bhop_GetDebug();

// Each engine is set independently.
void Bhop_SetEngine(int engine, bool on);
bool Bhop_GetEngine(int engine);

// Per-engine tuning.
void  Bhop_SetScrollInterval(float ms);   // SCROLL_SI / SCROLL_ME
float Bhop_ScrollInterval();
void  Bhop_SetDelayMs(float ms);          // KEY_SI_DEL, default one tick
float Bhop_DelayMs();
void  Bhop_SetInjectKey(int vk);          // KEY_SI / KEY_SI_DEL / FPS64
int   Bhop_InjectKey();
void  Bhop_SetFpsTarget(int fps);         // FPS64
int   Bhop_FpsTarget();
