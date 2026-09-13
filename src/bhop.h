// --- src/bhop.h ---
#pragma once
#include <cstdint>

// ══ READ THIS: WHICH MODE WRITES TO CS2 ══════════════════════════════════
//
//   BHOP_INJECT  ("Inject keys")      -> does NOT write to cs2.exe.
//                                        Uses SendInput. Zero memory writes,
//                                        but timing has OS jitter.
//
//   BHOP_MEMORY  ("Write jump button") -> WRITES to cs2.exe.
//                                        Two int32 writes per jump. This is
//                                        the precise mode: microsecond write
//                                        latency instead of SendInput jitter.
//
// A bhop cannot be done with zero writes AND be precise: reading memory cannot
// cause a jump, and the only offset-free way to cause one is keystroke
// injection, which is inherently nondeterministic. So the choice is real:
// what do you want, no writes or no misses?
//
// ══ HOW HOLD-SPACE WORKS ═════════════════════════════════════════════════
// Holding the physical key is a GATE, not the input. While bhop is driving, the
// keyboard hook swallows your physical spacebar so the game's jump input comes
// only from us. Without that, auto-repeat re-asserts your held DOWN against our
// release and the landing press edge never forms -- which is why holding space
// used to behave randomly while auto-jump (no key held) was perfect.
//
// THE JUMP PATTERN (both modes), which is the pattern you observed timing
// perfectly:
//     airborne  -> release   (guarantees the next press is a fresh edge)
//     grounded  -> press, and hold for ~2 ticks
// The hold matters: the engine rewrites the button from input every tick, so a
// press that lands just before the engine's own write would otherwise be
// erased. Holding across two ticks survives that race.
//
// GROUND STATE is graded against the local player's Z (m_vOldOrigin, verified):
//   z     - Z unchanged for ~22 ms -> standing. Works at any height, so a
//           landing off a roof reads exactly like a landing on flat ground.
//   flag  - once seen set while Z is static AND clear while Z moves it becomes
//           primary, being instance-accurate where the Z test needs a window.
//   hge   - same rule.
enum BhopMode : int {
    BHOP_OFF    = 0,
    BHOP_INJECT = 1,   // synthesised keystrokes -- NO writes to cs2.exe
    BHOP_MEMORY = 2,   // WRITES the jump button to cs2.exe (precise)
};

struct BhopDebug {
    int   mode        = BHOP_MEMORY;
    uintptr_t offset  = 0;
    bool  on_ground   = false;
    bool  focused     = false;
    bool  space_held  = false;   // PHYSICAL spacebar state
    bool  hook_ok     = false;   // low-level keyboard hook installed
    bool  driving     = false;   // currently commanding the jump
    bool  suppressing = false;   // physical spacebar is being swallowed
    int   signals     = 0;       // bit0 z, bit1 flag, bit2 hge
    int   presses     = 0;       // press edges generated this session
    bool  locked      = false;   // MEMORY mode: address confirmed
    bool  scanning    = false;
};

void Bhop_Init();
void Bhop_Shutdown();

BhopDebug Bhop_GetDebug();
void      Bhop_Rescan();          // MEMORY mode: forget the locked address

void Bhop_SetMode(int mode);      // BhopMode
int  Bhop_Mode();
