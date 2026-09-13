// --- src/bhop.h ---
#pragma once
#include <cstdint>

// Bhop has two backends:
//
//  * memory mode (default) - writes the CS2 jump button. Fast and clean, but
//    the button offset moves on every CS2 update, so it is resolved at runtime
//    by Bhop_DetectJump() instead of being hard-coded.
//
//  * input mode - synthesises a spacebar through SendInput. Needs no offsets
//    at all, so it survives any game update, but it depends on the game
//    accepting injected input and on CS2 being the foreground window.
void Bhop_Init();
void BhopTick();

// ── runtime jump-button discovery ─────────────────────────────────────────
// Hold W (or SPACE) while calling, so the scanner has a pressed slot to lock
// onto. Returns:
//    1 = found using the held key   (reliable)
//    0 = found structurally only    (verify with Bhop_LiveJumpValue())
//   -1 = not found                 (hold W or SPACE and try again)
int Bhop_DetectJump();

uintptr_t Bhop_JumpOffset();               // offset relative to client.dll
void      Bhop_SetJumpOffset(uintptr_t off);
uint32_t  Bhop_LiveJumpValue();            // live dword at the jump address
bool      Bhop_IsOnGround();

// ── input-mode fallback ───────────────────────────────────────────────────
void Bhop_SetInputMode(bool enabled);
bool Bhop_InputMode();
