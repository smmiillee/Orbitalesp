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

Memory      g_mem;
static ESP  g_esp;
static bool g_running   = true;
bool        g_menu_open = false;
HWND        g_cs2_hwnd  = nullptr;

struct Config {
    bool   esp_boxes      = true;
    bool   esp_health     = true;
    bool   esp_distance   = true;
    bool   esp_enemy_only = true;
    bool   esp_team       = false;
    float  box_thickness  = 1.5f;
    ImVec4 color_enemy    = { 1.0f, 0.10f, 0.10f, 1.0f };
    ImVec4 color_team     = { 0.10f, 1.0f, 0.20f, 1.0f };
    bool   bhop_enabled   = true;
} g_cfg;

static int g_screen_w = 1920;
static int g_screen_h = 1080;

static bool find_cs2_window() {
    g_cs2_hwnd = FindWindowA("SDL_app", nullptr);
    if (!g_cs2_hwnd) return false;
    RECT r{};
    if (!GetClientRect(g_cs2_hwnd, &r)) return false;
    if (r.right <= 0 || r.bottom <= 0)  return false;
    g_screen_w = r.right;
    g_screen_h = r.bottom;
    return true;
}

void memory_thread() {
    while (g_running && !g_mem.attach(L"cs2.exe"))
        std::this_thread::sleep_for(std::chrono::seconds(2));
    while (g_running) {
        if (g_mem.is_valid()) {
            g_esp.update(g_mem, g_mem.base_address);
            if (g_cfg.bhop_enabled) BhopTick();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

void render_esp(ImDrawList* dl) {
    for (const auto& p : g_esp.players) {
        // team 2 = CT, team 3 = T
        // enemy_only: skip teammates
        if (g_cfg.esp_enemy_only && !g_cfg.esp_team && p.team == 2) continue;
        if (!g_cfg.esp_enemy_only && !g_cfg.esp_team && p.team != 3) continue;

        ImVec4 col4 = (p.team == 3) ? g_cfg.color_enemy : g_cfg.color_team;
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
    const float WIN_W = 420.0f;
    const float WIN_H = 260.0f;
    const float COL_W = (WIN_W - 24.0f) / 2.0f; // two equal columns

    ImGui::SetNextWindowSize({ WIN_W, WIN_H }, ImGuiCond_Always);
    ImGui::SetNextWindowPos ({ 20.0f, 20.0f }, ImGuiCond_Once);
    ImGui::Begin("Orbital", nullptr,
        ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoScrollbar);

    // ── Status bar ───────────────────────────────────────────────────────────
    if (!g_mem.is_valid())
        ImGui::TextColored({ 1.0f, 0.5f, 0.0f, 1.0f }, "[ Waiting for CS2... ]");
    else
        ImGui::TextColored({ 0.0f, 0.7f, 0.0f, 1.0f },
            "[ Attached | %dx%d ]", g_screen_w, g_screen_h);
    ImGui::Separator();

    // ── Two equal columns — ESP left, Misc right ─────────────────────────────
    ImGui::Columns(2, "cols", true);
    ImGui::SetColumnWidth(0, COL_W);

    // LEFT — ESP
    ImGui::TextDisabled("ESP");
    ImGui::Separator();
    ImGui::Checkbox("Boxes",      &g_cfg.esp_boxes);
    ImGui::Checkbox("Health Bar", &g_cfg.esp_health);
    ImGui::Checkbox("Distance",   &g_cfg.esp_distance);
    ImGui::Checkbox("Enemy Only", &g_cfg.esp_enemy_only);
    ImGui::Checkbox("Show Team",  &g_cfg.esp_team);
    ImGui::Spacing();
    ImGui::TextDisabled("Colors");
    ImGui::Separator();
    ImGui::ColorEdit4("##ecol", &g_cfg.color_enemy.x,
        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
    ImGui::SameLine(); ImGui::Text("Enemy");
    ImGui::ColorEdit4("##tcol", &g_cfg.color_team.x,
        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
    ImGui::SameLine(); ImGui::Text("Team");
    ImGui::Spacing();
    ImGui::TextDisabled("Style");
    ImGui::Separator();
    ImGui::SetNextItemWidth(COL_W - 16.0f);
    ImGui::SliderFloat("##thick", &g_cfg.box_thickness, 0.5f, 4.0f, "%.1f px");

    // RIGHT — Misc
    ImGui::NextColumn();
    ImGui::TextDisabled("Misc");
    ImGui::Separator();
    ImGui::Checkbox("Bhop", &g_cfg.bhop_enabled);

    ImGui::Columns(1);
    ImGui::Separator();

    // ── Button row ───────────────────────────────────────────────────────────
    float btn_w = (WIN_W
        - ImGui::GetStyle().WindowPadding.x * 2.0f
        - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;

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
