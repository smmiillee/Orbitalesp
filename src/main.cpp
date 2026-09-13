// --- src/main.cpp ---
#include <Windows.h>
#include <chrono>
#include <cstdio>
#include <thread>
#include <vector>

#include <imgui.h>

#include "memory.h"
#include "esp.h"
#include "overlay.h"
#include "offsets.h"
#include "bhop.h"

static ESP g_esp;
static bool g_running = true;
bool g_menu_open = false;
bool g_vsync = false;          // false = uncapped overlay -> less mouse lag
HWND g_cs2_hwnd = nullptr;

struct Config {
    bool  esp_boxes        = true;
    bool  esp_health       = true;
    bool  esp_distance     = true;
    bool  esp_show_enemies = true;
    bool  esp_show_team    = true;
    bool  esp_skeleton     = true;
    bool  esp_head_dot     = true;
    float box_thickness    = 1.5f;
    ImVec4 color_enemy     = { 1.00f, 0.10f, 0.10f, 1.00f };
    ImVec4 color_team      = { 0.10f, 1.00f, 0.20f, 1.00f };
    ImVec4 color_skeleton  = { 1.00f, 1.00f, 1.00f, 0.90f };
    bool  bhop_enabled     = true;
} g_cfg;

static int g_screen_w   = 1920;
static int g_screen_h   = 1080;
static int g_local_team = 0;

// Skeleton bone connections (indices from BoneId in esp.h).
static constexpr int kSkeleton[][2] = {
    { BONE_HEAD,       BONE_NECK       },
    { BONE_NECK,       BONE_SPINE      },
    { BONE_SPINE,      BONE_PELVIS     },
    { BONE_SPINE,      BONE_L_SHOULDER },
    { BONE_L_SHOULDER, BONE_L_ARM      },
    { BONE_L_ARM,      BONE_L_HAND     },
    { BONE_SPINE,      BONE_R_SHOULDER },
    { BONE_R_SHOULDER, BONE_R_ARM      },
    { BONE_R_ARM,      BONE_R_HAND     },
    { BONE_PELVIS,     BONE_L_HIP      },
    { BONE_L_HIP,      BONE_L_KNEE     },
    { BONE_L_KNEE,     BONE_L_FOOT     },
    { BONE_PELVIS,     BONE_R_HIP      },
    { BONE_R_HIP,      BONE_R_KNEE     },
    { BONE_R_KNEE,     BONE_R_FOOT     },
};

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

// Reader thread: world positions, bones and bhop. No view matrix here.
void memory_thread() {
    while (g_running && !g_mem.attach(L"cs2.exe"))
        std::this_thread::sleep_for(std::chrono::seconds(2));

    Bhop_Init(); // was never called before -- part of why bhop did nothing

    while (g_running) {
        if (g_mem.is_valid()) {
            const uintptr_t local_pawn =
                g_mem.read<uintptr_t>(g_mem.client_dll + offsets::dwLocalPlayerPawn);
            if (local_pawn)
                g_local_team =
                    g_mem.read<int>(local_pawn + offsets::m_iTeamNum) & 0xFF;

            g_esp.update_world(g_mem, g_mem.client_dll);

            if (g_cfg.bhop_enabled) BhopTick();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
}

void render_esp(ImDrawList* dl) {
    if (!g_mem.is_valid()) return;

    // Fresh view matrix read this frame; world positions interpolated.
    std::vector<PlayerESP> players =
        g_esp.project(g_mem, g_mem.client_dll, g_screen_w, g_screen_h);

    for (const auto& p : players) {
        const bool is_teammate = (g_local_team != 0 && p.team == g_local_team);
        const bool is_enemy    = !is_teammate;

        if (is_enemy    && !g_cfg.esp_show_enemies) continue;
        if (is_teammate && !g_cfg.esp_show_team)    continue;

        const ImU32 col = ImGui::ColorConvertFloat4ToU32(
            is_enemy ? g_cfg.color_enemy : g_cfg.color_team);
        const ImU32 skel_col =
            ImGui::ColorConvertFloat4ToU32(g_cfg.color_skeleton);

        const float cx  = p.screen_head.x;
        const float top = p.screen_head.y;
        const float bot = p.screen_feet.y;
        const float bh  = p.box_h;
        const float bw  = p.box_w;

        if (cx + bw / 2.0f < 0 || cx - bw / 2.0f > g_screen_w) continue;
        if (bot < 0 || top > g_screen_h) continue;

        if (g_cfg.esp_boxes) {
            dl->AddRect(
                { cx - bw / 2.0f, top },
                { cx + bw / 2.0f, bot },
                col, 0.0f, 0, g_cfg.box_thickness);
        }

        // ── skeleton ──
        if (g_cfg.esp_skeleton && p.has_bones) {
            for (const auto& link : kSkeleton) {
                const int a = link[0];
                const int b = link[1];
                if (!p.bone_ok[a] || !p.bone_ok[b]) continue;
                dl->AddLine(
                    { p.bones[a].x, p.bones[a].y },
                    { p.bones[b].x, p.bones[b].y },
                    skel_col, g_cfg.box_thickness);
            }
        }

        // ── head dot ──
        // p.screen_head is the actual head bone when bones are readable, and
        // falls back to origin + 70 when they aren't.
        if (g_cfg.esp_head_dot) {
            float r = bh * 0.035f;
            if (r < 2.5f) r = 2.5f;
            if (r > 6.0f) r = 6.0f;
            dl->AddCircleFilled({ p.screen_head.x, p.screen_head.y }, r, col, 16);
        }

        // ── health bar ──
        if (g_cfg.esp_health) {
            const float bar_h = bh * (p.health / 100.0f);
            const float bar_x = cx - bw / 2.0f - 6.0f;

            const ImU32 hp_col = IM_COL32(
                (int)(255 * (1.0f - p.health / 100.0f)),
                (int)(255 * (p.health / 100.0f)),
                0, 255);

            dl->AddRectFilled({ bar_x, bot - bar_h }, { bar_x + 3.0f, bot }, hp_col);
            dl->AddRect     ({ bar_x, top },          { bar_x + 3.0f, bot },
                             IM_COL32(0, 0, 0, 180));
        }

        // ── distance (now measured from you, not from the map origin) ──
        if (g_cfg.esp_distance) {
            char buf[32];
            std::snprintf(buf, sizeof(buf), "%.0fm", p.distance);
            dl->AddText({ cx - 12.0f, top - 14.0f },
                        IM_COL32(255, 255, 255, 220), buf);
        }
    }
}

void render_menu() {
    ImGui::SetNextWindowSize({ 480.0f, 380.0f }, ImGuiCond_Once);
    ImGui::SetNextWindowSizeConstraints({ 340.0f, 260.0f }, { 900.0f, 720.0f });
    ImGui::SetNextWindowPos({ 20.0f, 20.0f }, ImGuiCond_Once);
    ImGui::Begin("Orbital", nullptr, ImGuiWindowFlags_NoScrollbar);

    const float win_w = ImGui::GetContentRegionAvail().x;

    if (!g_mem.is_valid())
        ImGui::TextColored({ 1.0f, 0.5f, 0.0f, 1.0f }, "[ Waiting for CS2... ]");
    else
        ImGui::TextColored({ 0.0f, 0.7f, 0.0f, 1.0f },
            "[ Attached | %dx%d | Team:%d ]", g_screen_w, g_screen_h, g_local_team);

    ImGui::Separator();

    const float col_w = win_w / 2.0f;
    ImGui::Columns(2, nullptr, false);
    ImGui::SetColumnWidth(0, col_w);

    ImGui::TextDisabled("[ ESP ]");
    ImGui::Checkbox("Boxes", &g_cfg.esp_boxes);
    ImGui::Checkbox("Health Bar", &g_cfg.esp_health);
    ImGui::Checkbox("Distance", &g_cfg.esp_distance);
    ImGui::Checkbox("Show Enemies", &g_cfg.esp_show_enemies);
    ImGui::Checkbox("Show Team", &g_cfg.esp_show_team);

    ImGui::Spacing();
    ImGui::TextDisabled("[ Skeleton ]");
    ImGui::Checkbox("Skeleton", &g_cfg.esp_skeleton);
    ImGui::Checkbox("Head Dot", &g_cfg.esp_head_dot);

    ImGui::Spacing();
    ImGui::TextDisabled("[ Colors ]");
    ImGui::ColorEdit4("##ec", &g_cfg.color_enemy.x,
        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
    ImGui::SameLine(); ImGui::Text("Enemy");

    ImGui::ColorEdit4("##tc", &g_cfg.color_team.x,
        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
    ImGui::SameLine(); ImGui::Text("Team");

    ImGui::ColorEdit4("##sc", &g_cfg.color_skeleton.x,
        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
    ImGui::SameLine(); ImGui::Text("Skeleton");

    ImGui::Spacing();
    ImGui::TextDisabled("[ Style ]");
    ImGui::SetNextItemWidth(col_w - 16.0f);
    ImGui::SliderFloat("##thick", &g_cfg.box_thickness, 0.5f, 4.0f, "%.1f px");

    ImGui::NextColumn();

    ImGui::TextDisabled("[ Misc ]");
    ImGui::Checkbox("Bhop", &g_cfg.bhop_enabled);
    ImGui::Checkbox("V-Sync (off = less lag)", &g_vsync);

    ImGui::Columns(1);
    ImGui::Separator();

    const float btn_w =
        (win_w - ImGui::GetStyle().ItemSpacing.x * 2.0f) / 3.0f;

    if (ImGui::Button("[Apply]", { btn_w, 0 })) {}
    ImGui::SameLine();
    if (ImGui::Button("[Reset]", { btn_w, 0 })) g_cfg = Config{};
    ImGui::SameLine();
    if (ImGui::Button("[Exit]", { btn_w, 0 })) g_running = false;

    ImGui::Spacing();
    ImGui::TextDisabled("INSERT - menu    F9 - exit");
    ImGui::TextDisabled("ESP choppy in game? run:  engine_no_focus_sleep 0");

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
        if (GetAsyncKeyState(VK_F9) & 1)     g_running   = false;
        if (GetAsyncKeyState(VK_END) & 1)    g_running   = false;

        overlay.begin_frame();
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        render_esp(dl);
        if (g_menu_open) render_menu();
        overlay.end_frame();

        // With vsync off the overlay would spin at thousands of fps. Cap it
        // at ~240 so it stays fast and low-latency without pegging the GPU.
        if (!g_vsync) {
            static auto next = std::chrono::steady_clock::now();
            next += std::chrono::microseconds(4167); // 240 Hz
            const auto now = std::chrono::steady_clock::now();
            if (next > now) std::this_thread::sleep_for(next - now);
            else            next = now;
        }
    }

    g_running = false;
    mem_t.join();
    overlay.cleanup();
    g_mem.detach();
    return 0;
}
