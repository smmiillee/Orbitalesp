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

static Memory g_mem;
static ESP    g_esp;
static bool   g_running = true;

struct Config {
    bool   esp_boxes     = true;
    bool   esp_health    = true;
    bool   esp_distance  = true;
    bool   enemy_only    = true;
    float  box_thickness = 1.5f;
    ImVec4 color_enemy   = { 1.0f, 0.2f, 0.2f, 1.0f };
    ImVec4 color_team    = { 0.2f, 1.0f, 0.4f, 1.0f };
} g_cfg;

bool g_menu_open = false;

static int g_screen_w = 1920;
static int g_screen_h = 1080;

void memory_thread() {
    while (g_running) {
        if (g_mem.is_valid()) {
            g_esp.update_world(g_mem, g_mem.base_address);
            BhopTick();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void render_esp(ImDrawList* dl, int screen_w, int screen_h) {
    auto players = g_esp.project(g_mem, g_mem.base_address, screen_w, screen_h);

    for (const auto& p : players) {
        if (g_cfg.enemy_only && p.team == 2) continue;

        ImVec4 col4 = (p.team == 3) ? g_cfg.color_enemy : g_cfg.color_team;
        ImU32  col  = ImGui::ColorConvertFloat4ToU32(col4);

        float cx  = p.screen_head.x;
        float top = p.screen_top.y;
        float bot = p.screen_feet.y;
        float bh  = bot - top;
        float bw  = bh * 0.45f;

        if (cx < 0 || cx > screen_w || top < 0 || bot > screen_h) continue;

        if (g_cfg.esp_boxes) {
            dl->AddRect(
                { cx - bw / 2.0f, top },
                { cx + bw / 2.0f, bot },
                col, 0.0f, 0, g_cfg.box_thickness
            );
        }

        float label_y = top - 14.0f;

        if (g_cfg.esp_health) {
            float bar_h  = bh * (p.health / 100.0f);
            float bar_x  = cx - bw / 2.0f - 6.0f;
            ImU32 hp_col = IM_COL32(
                (int)(255 * (1.0f - p.health / 100.0f)),
                (int)(255 * (p.health / 100.0f)),
                0, 255
            );
            dl->AddRectFilled({ bar_x, bot - bar_h }, { bar_x + 3.0f, bot }, hp_col);
            dl->AddRect({ bar_x, top }, { bar_x + 3.0f, bot }, IM_COL32(0, 0, 0, 180));
        }

        if (g_cfg.esp_distance) {
            char buf[32];
            snprintf(buf, sizeof(buf), "%.0fm", p.distance);
            dl->AddText({ cx - 12.0f, label_y }, IM_COL32(255, 255, 255, 200), buf);
        }
    }

    // Bomb
    BombESP bomb = g_esp.project_bomb(g_mem, g_mem.base_address, screen_w, screen_h);
    if (bomb.active) {
        dl->AddCircle({ bomb.screen.x, bomb.screen.y }, 8.0f,
            IM_COL32(255, 200, 0, 255), 16, 2.0f);
        dl->AddText({ bomb.screen.x + 10.0f, bomb.screen.y - 7.0f },
            IM_COL32(255, 200, 0, 255), "[C4]");
    }
}

void render_menu() {
    ImGui::SetNextWindowSize({ 300, 380 }, ImGuiCond_Once);
    ImGui::SetNextWindowPos({ 30, 30 }, ImGuiCond_Once);
    ImGui::Begin("Orbital", nullptr, ImGuiWindowFlags_NoResize);

    if (!g_mem.is_valid())
        ImGui::TextColored({ 1.0f, 0.4f, 0.4f, 1.0f }, "CS2 not found");
    else
        ImGui::TextColored({ 0.4f, 1.0f, 0.4f, 1.0f }, "Attached");

    ImGui::SeparatorText("ESP");
    ImGui::Checkbox("Boxes",      &g_cfg.esp_boxes);
    ImGui::Checkbox("Health",     &g_cfg.esp_health);
    ImGui::Checkbox("Distance",   &g_cfg.esp_distance);
    ImGui::Checkbox("Enemy Only", &g_cfg.enemy_only);

    ImGui::SeparatorText("Colors");
    ImGui::ColorEdit4("Enemy", &g_cfg.color_enemy.x, ImGuiColorEditFlags_NoInputs);
    ImGui::ColorEdit4("Team",  &g_cfg.color_team.x,  ImGuiColorEditFlags_NoInputs);

    ImGui::SeparatorText("Style");
    ImGui::SliderFloat("Thickness", &g_cfg.box_thickness, 0.5f, 4.0f);

    ImGui::SeparatorText("Bhop");
    ImGui::Checkbox("Enabled (hold SPACE)", &g_bhop_cfg.enabled);

    ImGui::BeginDisabled(!g_bhop_cfg.enabled);
    if (ImGui::RadioButton("Standard", g_bhop_cfg.mode == 0)) g_bhop_cfg.mode = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("64fps",    g_bhop_cfg.mode == 1)) g_bhop_cfg.mode = 1;

    if (g_bhop_cfg.mode == 0)
        ImGui::TextDisabled("1ms timer + scroll on land");
    else {
        ImGui::TextDisabled("fps_max 64 + scroll on land");
        ImGui::TextColored({ 1.0f, 0.85f, 0.0f, 1.0f }, "Caps framerate to 64!");
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::TextDisabled("INSERT - menu   END/F9 - exit");

    ImGui::End();
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    while (!g_mem.attach(L"cs2.exe"))
        std::this_thread::sleep_for(std::chrono::seconds(2));

    HWND cs2 = FindWindowA("SDL_app", nullptr);
    if (cs2) {
        RECT r{};
        if (GetClientRect(cs2, &r) && r.right > 0) {
            g_screen_w = r.right;
            g_screen_h = r.bottom;
        }
    }

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
        if (GetAsyncKeyState(VK_END)    & 1) g_running   = false;
        if (GetAsyncKeyState(VK_F9)     & 1) g_running   = false;

        overlay.begin_frame();
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        render_esp(dl, g_screen_w, g_screen_h);
        if (g_menu_open) render_menu();
        overlay.end_frame();
    }

    g_running = false;
    mem_t.join();
    overlay.cleanup();
    g_mem.detach();
    return 0;
}
