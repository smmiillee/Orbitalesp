// --- src/bhop.h ---
#pragma once
#include <cstdint>

struct BhopDebug {
    uintptr_t offset    = 0;
    bool  on_ground     = false;
    bool  focused       = false;
    int   signals       = 0;   // bit0 z, bit1 flag, bit2 hge
    bool  locked        = false;
    bool  scanning      = false;
    int   candidates    = 0;
};

void Bhop_Init();
void Bhop_Shutdown();

BhopDebug Bhop_GetDebug();
void      Bhop_Rescan();

void Bhop_SetInputMode(bool enabled);
bool Bhop_InputMode();

void Bhop_SetEnabled(bool enabled);
