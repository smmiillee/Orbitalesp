#include <Windows.h>
#include <thread>
#include <chrono>
#include <vector>
#include <cstdio>

#include "memory.h"
#include "offsets.h"
#include "cs2math.h"
#include "sdk.h"
#include "esp.h"
#include "bhop.h"
#include "overlay.h"
#include "imgui.h"

static std::vector<Player> g_Players;
static int  g_LocalTeam  = 0;
static bool g_EspEnabled  = true;
static bool g_BhopEnabled = true;
static OverlayContext g_Overlay;

// Debug values shown in menu
static uintptr_t dbg_LocalPawn    = 0;
static int       dbg_LocalHealth  = 0;
static int       dbg_PlayerCount  = 0;
static uintptr_t dbg_ClientBase   = 0;

void MemoryThread() {
    while (g_Overlay.running) {
        dbg_ClientBase = g_Mem.clientBase;

        uintptr_t localPawn = g_Mem.Read<uintptr_t>(
            g_Mem.clientBase + offsets::dwLocalPlayerPawn);
        dbg_LocalPawn = localPawn;

        if (localPawn) {
            dbg_LocalHealth = g_Mem.Read<int>(localPawn + cs2::m_iHealth);
            g_LocalTeam     = g_Mem.Read<int>(localPawn + cs2::m_iTeamNum);

            Matrix4x4 vm = g_Mem.Read<Matrix4x4>(
                g_Mem.clientBase + offsets::dwViewMatrix);

            if (g_EspEnabled) {
                g_Players = GetPlayers(vm, g_Overlay.width, g_Overlay.height);
                dbg_PlayerCount = (int)g_Players.size();
            } else {
                g_Players.clear();
                dbg_PlayerCount = 0;
            }
        }

        if (g_BhopEnabled)
            BhopTick();

        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    // Wait for CS2
    printf("[*] Waiting for CS2...\n");
    while (!FindWindowA("SDL_app", nullptr))
        Sleep(1000);
    printf("[*] CS2 found.\n");

    // Attach memory
    printf("[*] Attaching to cs2.exe...\n");
    while (!g_Mem.Attach("cs2.exe"))
        Sleep(1000);
    printf("[*] Attached OK.\n");

    // Get client.dll
    printf("[*] Finding client.dll...\n");
    while (!g_Mem.GetModule("client.dll"))
        Sleep(1000);
    printf("[*] client.dll base: 0x%llX\n", (unsigned long long)g_Mem.clientBase);

    if (!OverlayCreate(g_Overlay))
        return 1;

    std::thread memThread(MemoryThread);
    memThread.detach();

    OverlayRun(g_Overlay, []() {
    DrawESP(g_Players, g_LocalTeam);

    if (g_Overlay.menuOpen) {
        ImGui::SetNextWindowSize(ImVec2(320, 320), ImGuiCond_Once);
        ImGui::SetNextWindowPos(ImVec2(20, 20), ImGuiCond_Once);
        ImGui::Begin("cs2external", nullptr,
            ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse);

        ImGui::SeparatorText("Features");
        ImGui::Checkbox("ESP",  &g_EspEnabled);
        ImGui::Checkbox("Bhop", &g_BhopEnabled);

        ImGui::Spacing();
        ImGui::SeparatorText("Debug");
        ImGui::Text("client.dll : 0x%llX", (unsigned long long)dbg_ClientBase);
        ImGui::Text("LocalPawn  : 0x%llX", (unsigned long long)dbg_LocalPawn);
        ImGui::Text("LocalHP    : %d",      dbg_LocalHealth);
        ImGui::Text("Players    : %d",      dbg_PlayerCount);

        // Bone array probe
        if (dbg_LocalPawn) {
            uintptr_t sceneNode  = g_Mem.Read<uintptr_t>(dbg_LocalPawn + cs2::m_pGameSceneNode);
            uintptr_t modelState = sceneNode + cs2::m_modelState;

            // Probe four candidate bone array offsets
            uintptr_t b0x70 = g_Mem.Read<uintptr_t>(modelState + 0x70);
            uintptr_t b0x80 = g_Mem.Read<uintptr_t>(modelState + 0x80);
            uintptr_t b0x90 = g_Mem.Read<uintptr_t>(modelState + 0x90);
            uintptr_t b0xA0 = g_Mem.Read<uintptr_t>(modelState + 0xA0);

            ImGui::Spacing();
            ImGui::SeparatorText("Bone Probe");
            ImGui::Text("SceneNode  : 0x%llX", (unsigned long long)sceneNode);
            ImGui::Text("ModelState : 0x%llX", (unsigned long long)modelState);
            ImGui::Text("+0x70 : 0x%llX", (unsigned long long)b0x70);
            ImGui::Text("+0x80 : 0x%llX", (unsigned long long)b0x80);
            ImGui::Text("+0x90 : 0x%llX", (unsigned long long)b0x90);
            ImGui::Text("+0xA0 : 0x%llX", (unsigned long long)b0xA0);
        }

        ImGui::Spacing();
        ImGui::TextDisabled("INSERT = toggle menu");
        ImGui::TextDisabled("F9 = exit");

        ImGui::End();
    }
});
