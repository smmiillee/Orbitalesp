#pragma once

// Bhop, ESP, and overlay initialization functions
void Bhop_Init();
void Bhop_Tick();

void ESP_Init();
void ESP_Update();
void ESP_Render(ImDrawList* dl, int screen_w, int screen_h);

void Overlay_Init();
void Overlay_DrawFrame();
