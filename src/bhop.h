// --- src/bhop.h ---
#pragma once

// Bhop via the CS2 button-system write.
// Offsets (jump button 0xB3E00, m_hGroundEntity 0x50C) are verified working
// for the current build — do not change.
void Bhop_Init();
void BhopTick();
