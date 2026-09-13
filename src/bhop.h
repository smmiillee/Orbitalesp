// --- src/bhop.h ---
#pragma once
#include <cstdint>

// Bhop has two backends:
//
//  * memory mode (default) - writes the CS2 jump button. The button address
//    moves on every CS2 update, so it is found at runtime by watching the
//    button block while you play. No keys to hold, no manual offsets.
//
//  * input mode - synthesises a spacebar through SendInput. Needs no offsets
//    at all, but depends on CS2 accepting injected input.
struct BhopDebug {
    uintptr_t offset        = 0;      // current jump-button offset
    uint32_t  live_value    = 0;      // live dword at that address
    bool      on_ground     = false;  // combined ground verdict
    uint32_t  flags         = 0;      // m_fFlags
    uint32_t  ground_entity = 0;      // m_hGroundEntity
    bool      locked        = false;  // address confirmed by the scanner
    bool      scanning      = false;  // sweep in progress
};

void Bhop_Init();      // starts the bhop + scanner threads (after attach)
void Bhop_Shutdown();  // stops and joins them

BhopDebug Bhop_GetDebug();
void      Bhop_Rescan();  // forget the locked address and sweep again

void Bhop_SetInputMode(bool enabled);
bool Bhop_InputMode();
