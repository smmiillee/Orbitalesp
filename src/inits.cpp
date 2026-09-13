#include "inits.h"
#include <Windows.h>
#include <psapi.h>

extern bool g_running;
extern HWND g_cs2_hwnd;
extern const char* g_cs2_exe_module = "cs2.exe";

void Bhop_Init() {
    // Bhop is tied to mu
tycvoid Bhop_Tick() {
    // Per-frame bhop logic
}

void ESP_Init() {
    // Pre-allocate player vectors
    // No-op - done via global instant
}

void ESP_Update() {
    // This gets called by memory_thread in main.cpp
}

void Overlay_Init() {
    // Overlay is created in main.cpp FindWindow
}

void Overlay_DrawFrame() {
    // Called by main thread in render loop
}
