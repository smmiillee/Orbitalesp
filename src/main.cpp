#include <Windows.h>
#include <thread>
#include <chrono>
#include <vector>

#include "memory.h"
#include "offsets.h"
#include "math.h"
#include "sdk.h"
#include "esp.h"
#include "bhop.h"
#include "overlay.h"

static std::vector<Player> g_Players;
static int g_LocalTeam = 0;
static OverlayContext g_Overlay;

// Background thread: reads game memory at ~128Hz, feeds g_Players
void MemoryThread() {
    while (g_Overlay.running) {
        uintptr_t localPawn = g_Mem.Read<uintptr_t>(
            g_Mem.clientBase + offsets::dwLocalPlayerPawn);

        if (localPawn) {
            g_LocalTeam = g_Mem.Read<int>(localPawn + cs2::m_iTeamNum);

            // View matrix
            Matrix4x4 vm = g_Mem.Read<Matrix4x4>(
                g_Mem.clientBase + offsets::dwViewMatrix);

            g_Players = GetPlayers(vm, g_Overlay.width, g_Overlay.height);
        }

        BhopTick();

        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // Wait for CS2
    while (!FindWindowA("SDL_app", nullptr))
        Sleep(1000);

    // Attach to cs2.exe
    while (!g_Mem.Attach("cs2.exe"))
        Sleep(1000);

    // Get client.dll base
    while (!g_Mem.GetModule("client.dll"))
        Sleep(1000);

    if (!OverlayCreate(g_Overlay))
        return 1;

    // Spin up memory reader
    std::thread memThread(MemoryThread);
    memThread.detach();

    // Render loop
    OverlayRun(g_Overlay, []() {
        DrawESP(g_Players, g_LocalTeam);
    });

    OverlayDestroy(g_Overlay);
    return 0;
}
