// --- src/bhop.h ---
#pragma once

struct BhopConfig {
    bool enabled = false;
    int  mode    = 0;  // 0 = Standard, 1 = 64fps
};

extern BhopConfig g_bhop_cfg;

void BhopTick();
