// --- src/main.cpp ---
#include <Windows.h>
#include <thread>
#include <chrono>
#include <imgui.h>
#include "memory.h"
#include "esp.h"
#include "overlay.h"
#include "offsets.h"
#include "bhop.h"

static ESP  g_esp;
static bool g_running   = true;
bool        g_menu_open = false;
HWND        g_cs2_hwnd  = nullptr;

struct Config {
    bool   esp_boxes         = true;
    bool   esp_health        = true;
    bool   esp_distance      = true;
    bool   esp_show_enemies  = true;
    bool   esp_show_team     = true;
    float  box_thickness     = 1.5f;
    ImVec4 color_enemy       = { 1.0f, 0.10f, 0.10f, 1.0f };
    ImVec4 color_team        = { 0.10f, 1.0f, 0.20f, 1.0f };
    bool   bhop_enabled      = true;
} g_cfg;

static int  g_screen_w   = 1920;
static int  g_screen_h   = 1080;
static int  g_local_team = 0;

static bool find_cs2_window() {
    g_cs2_hwnd = FindWindowA("SDL_app", nullptr);
    if (!g_cs2_hwnd) return false;
    RECT r{};
    if (!GetClientRect(g_cs2_hwnd, &r)) return false;
    if (r.right <= 0 || r.bottom <= 0) return false;
    g_screen_w = r.right;
    g_screen_h = r.bottom;
    return true;
}

void memory_thread() {
    while (g_running && !g_mem.attach(L"cs2.exe"))
        std::this_thread::sleep_for(std::chrono::seconds(2));

    while (g_running) {
        if (g_mem.is_valid()) {
            uintptr_t local_pawn = g_mem.read<uintptr_t>(
                g_mem.client_dll + offsets::dwLocalPlayerPawn);
            if (local_pawn)
                g_local_team = g_mem.read<int>(local_pawn + offsets::m_iTeamNum) & 0xFF;

            // Pass actual screen resolution so projection is correct
            g_esp.update(g_mem, g_mem.client_dll, g_screen_w, g_screen_h);

            if (g_cfg.bhop_enabled) BhopTick();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void render_esp(ImDrawList* dl) {
    for (const auto& p : g_esp.players) {
        bool is_teammate = (g_local_team != 0 && p.team == g_local_team);
        bool is_enemy    = !is_teammate;

        if (is_enemy    && !g_cfg.esp_show_enemies) continue;
        if (is_teammate && !g_cfg.esp_show_team)    continue;

        ImVec4 col4 = is_enemy ? g_cfg.color_enemy : g_cfg.color_team;
        ImU32  col  = ImGui::ColorConvertFloat4ToU32(col4);

        float cx  = p.screen_head.x;
        float top = p.screen_head.y;
        float bot = p.screen_feet.y;
        float bh  = bot - top;
        float bw  = bh * 0.45f;

        if (bh < 5.0f) continue;
        if (cx + bw / 2.0f < 0 || cx - bw / 2.0f > g_screen_w) continue;
        if (bot < 0 || top > g_screen_h) continue;

        if (g_cfg.esp_boxes) {
            dl->AddRect(
                { cx - bw / 2.0f, top },
                { cx + bw / 2.0f, bot },
                col, 0.0f, 0, g_cfg.box_thickness
            );
        }

        if (g_cfg.esp_health) {
            float bar_h  = bh * (p.health / 100.0f);
            float bar_x  = cx - bw / 2.0f - 6.0f;
            ImU32 hp_col = IM_COL32(
                (int)(255 * (1.0f - p.health / 100.0f)),
                (int)(255 * (p.health / 100.0f)),
                0, 255
            );
            dl->AddRectFilled({ bar_x, bot - bar_h }, { bar_x + 3.0f, bot }, hp_col);
            dl->AddRect      ({ bar_x, top         }, { bar_x + 3.0f, bot }, IM_COL32(0,0,0,180));
        }

        if (g_cfg.esp_distance) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.0fm", p.distance);
            dl->AddText({ cx - 12.0f, top - 14.0f }, IM_COL32(255,255,255,220), buf);
        }
    }
}

void render_menu() {
    ImGui::SetNextWindowSize({ 420.0f, 340.0f }, ImGuiCond_Once);
    ImGui::SetNextWindowSizeConstraints({ 340.0f, 260.0f }, { 800.0f, 600.0f });
    ImGui::SetNextWindowPos({ 20.0f, 20.0f }, ImGuiCond_Once);
    ImGui::Begin("Orbital", nullptr, ImGuiWindowFlags_NoScrollbar);

    float win_w = ImGui::GetContentRegionAvail().x;

    if (!g_mem.is_valid())
        ImGui::TextColored({ 1.0f,0.5f,0.0f,1.0f }, "[ Waiting for CS2... ]");
    else
        ImGui::TextColored({ 0.0f,0.7f,0.0f,1.0f },
            "[ Attached | %dx%d | Team:%d ]", g_screen_w, g_screen_h, g_local_team);

    ImGui::Separator();

    float col_w = win_w / 2.0f;
    ImGui::Columns(2, nullptr, false);
    ImGui::SetColumnWidth(0, col_w);

    ImGui::TextDisabled("[ ESP ]");
    ImGui::Checkbox("Boxes",        &g_cfg.esp_boxes);
    ImGui::Checkbox("Health Bar",   &g_cfg.esp_health);
    ImGui::Checkbox("Distance",     &g_cfg.esp_distance);
    ImGui::Checkbox("Show Enemies", &g_cfg.esp_show_enemies);
    ImGui::Checkbox("Show Team",    &g_cfg.esp_show_team);
    ImGui::Spacing();
    ImGui::TextDisabled("[ Colors ]");
    ImGui::ColorEdit4("##ec", &g_cfg.color_enemy.x,
        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
    ImGui::SameLine(); ImGui::Text("Enemy");
    ImGui::ColorEdit4("##tc", &g_cfg.color_team.x,
        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
    ImGui::SameLine(); ImGui::Text("Team");
    ImGui::Spacing();
    ImGui::TextDisabled("[ Style ]");
    ImGui::SetNextItemWidth(col_w - 16.0f);
    ImGui::SliderFloat("##thick", &g_cfg.box_thickness, 0.5f, 4.0f, "%.1f px");

    ImGui::NextColumn();
    ImGui::TextDisabled("[ Misc ]");
    ImGui::Checkbox("Bhop", &g_cfg.bhop_enabled);
    ImGui::Spacing();
    ImGui::TextDisabled("[ Debug ]");
    ImGui::Text("Controllers: %d", g_esp.debug_total_controllers);
    ImGui::Text("Valid: %d off=0x%X str=%d", g_esp.debug_valid_pawns,
        g_esp.debug_sample_health, g_esp.debug_sample_team);
    ImGui::Text("Alive:      %d", g_esp.debug_alive);
    ImGui::Text("Positioned: %d", g_esp.debug_positioned);
    ImGui::Text("On screen:  %d", g_esp.debug_on_screen);
    ImGui::Text("Drawing:    %d", (int)g_esp.players.size());

    ImGui::Columns(1);
    ImGui::Separator();

    float btn_w = (win_w - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;
    if (ImGui::Button("[Apply]", { btn_w, 0 })) {}
    ImGui::SameLine();
    if (ImGui::Button("[Reset]", { btn_w, 0 })) g_cfg = {};
    ImGui::SameLine();
    if (ImGui::Button("[Exit]",  { btn_w, 0 })) g_running = false;

    ImGui::Spacing();
    ImGui::TextDisabled("INSERT - menu    F9 - exit");
    ImGui::End();
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    while (!find_cs2_window())
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

    Overlay overlay;
    if (!overlay.create(g_screen_w, g_screen_h)) return 1;

    std::thread mem_t(memory_thread);

    MSG msg{};
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) g_running = false;
        }
        if (GetAsyncKeyState(VK_INSERT) & 1) g_menu_open = !g_menu_open;
        if (GetAsyncKeyState(VK_F9)     & 1) g_running   = false;
        if (GetAsyncKeyState(VK_END)    & 1) g_running   = false;

        overlay.begin_frame();
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        render_esp(dl);
        if (g_menu_open) render_menu();
        overlay.end_frame();
    }

    g_running = false;
    mem_t.join();
    overlay.cleanup();
    g_mem.detach();
    return 0;
}
