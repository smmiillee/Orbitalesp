// --- src/bhop.h ---
#pragma once

// Bhop via a direct write to the CS2 jump button (dwForceJump).
// The button offset now lives in offsets.h -> offsets::dwForceJump, so it can
// be updated in one place after a game update. See bhop.cpp for the details.
void Bhop_Init();
void BhopTick();
