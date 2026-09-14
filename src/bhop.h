// --- src/bhop.h ---
#pragma once
#include <cstdint>

// Bhop by INPUT INJECTION ONLY. Nothing in this file writes to cs2.exe, and the
// project as a whole performs zero writes to the game process.
//
// ══ THE TWO TECHNIQUES, BOTH GATED ON HOLDING SPACE ══════════════════════
//
//  SCROLL (default, recommended) -- repeatedly injects mouse-wheel events while
//      the gate is held. A wheel event is itself a press+release pair for a
//      `+jump` bind, and we emit several per game tick, so one of them lands in
//      the window the game allows for a re-jump on landing.
//
//      This is why scroll beats prediction: prediction has to hit one specific
//      tick and its error is as wide as the window, whereas spamming covers
//      every phase of the tick instead of trying to hit one.
//
//  KEY EDGE (optional) -- presses/releases a key once per observed landing.
//      Kept as a second chance, not as the primary path.
//
// ══ WHY YOUR PHYSICAL KEY IS SWALLOWED WHILE DRIVING ═════════════════════
// Holding space makes the engine's own +jump stay down. A button that is
// already down cannot produce a new press edge, so a held key would block every
// jump we try to inject. While bhop is driving, the low-level keyboard hook
// swallows your physical spacebar: your held key becomes the GATE, and the
// edges come from us.
//
// ══ GROUND STATE ═════════════════════════════════════════════════════════
// Z comes from m_vOldOrigin, which is verified working. m_fFlags and
// m_hGroundEntity are not verified for this build, so each is only trusted
// after it has been seen set while Z is static AND clear while Z is moving --
// a wrong offset then degrades the reading rather than breaking it.
struct BhopDebug {
    // config
    bool enabled     = true;
    bool scroll      = true;
    bool key_inject  = false;

    // live state
    bool on_ground   = false;
    bool focused     = false;
    bool space_held  = false;   // PHYSICAL spacebar state
    bool hook_ok     = false;   // low-level keyboard hook installed
    bool driving     = false;   // currently injecting
    bool suppressing = false;   // physical spacebar is being swallowed

    int  signals     = 0;       // bit0 z, bit1 flag, bit2 hge
    int  scrolls     = 0;       // wheel events injected
    int  edges       = 0;       // key press edges injected
};

void Bhop_Init();
void Bhop_Shutdown();

BhopDebug Bhop_GetDebug();

void  Bhop_SetEnabled(bool on);
void  Bhop_SetScroll(bool on);
void  Bhop_SetKeyInject(bool on);
void  Bhop_SetScrollInterval(float ms);
float Bhop_ScrollInterval();
