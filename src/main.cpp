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
bool g_vsync = false;
HWND g_cs2_hwnd = nullptr;

struct Config {
    // ESP visuals
    bool  esp_boxes    = true;
    bool  esp_skeleton = true;
    bool  esp_head_dot = true;
    bool  esp_name     = true;
    bool  esp_weapon   = true;
    bool  esp_health   = true;
    bool  esp_distance = true;
    bool  esp_bomb     = true;
    // Teammates are OFF by default -- most people only want enemies.
    bool  esp_show_enemies = true;
    bool  esp_show_team    = false;
    float box_thickness = 0.5f;

    // Colours -- one per visual.
    bool   team_colors   = false;
    ImVec4 color_enemy   = { 1.00f, 0.15f, 0.15f, 1.00f };
    ImVec4 color_team    = { 0.20f, 0.90f, 0.35f, 1.00f };
    ImVec4 color_box     = { 1.00f, 0.15f, 0.15f, 1.00f };
    ImVec4 color_skel    = { 0.95f, 0.95f, 0.95f, 0.90f };
    ImVec4 color_head    = { 1.00f, 1.00f, 1.00f, 1.00f };
    ImVec4 color_name    = { 1.00f, 1.00f, 1.00f, 1.00f };
    ImVec4 color_weapon  = { 1.00f, 0.85f, 0.30f, 1.00f };
    ImVec4 color_dist    = { 0.85f, 0.85f, 0.85f, 0.90f };
    ImVec4 color_bomb    = { 1.00f, 0.45f, 0.00f, 1.00f };
    ImVec4 color_carrier = { 1.00f, 0.25f, 0.95f, 1.00f };

    // Bhop -- keystroke injection only, so nothing here writes to cs2.exe.
    bool  bhop_enabled      = true;
    bool  bhop_predict      = false;
    bool  bhop_separate_key = false;
    float bhop_lead_ms      = 16.0f;
} g_cfg;

static int  g_screen_w = 1920;
static int  g_screen_h = 1080;
static int  g_local_team = 0;
static int  g_detected_hz = 0;
static bool g_limit_fps = true;
static int  g_fps_override = 0;

static int query_refresh_rate(HWND game) {
    if (!game) return 0;
    HMONITOR mon = MonitorFromWindow(game, MONITOR_DEFAULTTONEAREST);
    MONITORINFOEXW mi{};
    mi.cbSize = sizeof(mi);
    if (!GetMonitorInfoW(mon, &mi)) return 0;

    DEVMODEW dm{};
    dm.dmSize = sizeof(dm);
    if (!EnumDisplaySettingsW(mi.szDevice, ENUM_CURRENT_SETTINGS, &dm)) return 0;

    const int hz = static_cast<int>(dm.dmDisplayFrequency);
    return (hz < 30 || hz > 1000) ? 0 : hz;
}

// sleep_for() on Windows quantises to the ~15.6 ms timer, so a 4 ms cap
// silently ran at 64 Hz. Sleep the bulk, spin the last stretch.
static void wait_until(std::chrono::steady_clock::time_point deadline) {
    for (;;) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return;
        const auto left = deadline - now;
        if (left > std::chrono::milliseconds(2))
            std::this_thread::sleep_for(left - std::chrono::milliseconds(1));
        else
            std::this_thread::yield();
    }
}

// Skeleton links, using the current (post animgraph_2_beta) bone map.
static constexpr int kSkeleton[][2] = {
    { BONE_PELVIS,     BONE_SPINE_1    },
    { BONE_SPINE_1,    BONE_SPINE_2    },
    { BONE_SPINE_2,    BONE_CHEST      },
    { BONE_CHEST,      BONE_NECK       },
    { BONE_NECK,       BONE_HEAD       },
    { BONE_NECK,       BONE_L_SHOULDER },
    { BONE_L_SHOULDER, BONE_L_ELBOW    },
    { BONE_L_ELBOW,    BONE_L_HAND     },
    { BONE_NECK,       BONE_R_SHOULDER },
    { BONE_R_SHOULDER, BONE_R_ELBOW    },
    { BONE_R_ELBOW,    BONE_R_HAND     },
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

// Reader thread: world samples. Bhop owns its own thread.
void memory_thread() {
    while (g_running && !g_mem.attach(L"cs2.exe"))
        wait_until(std::chrono::steady_clock::now() + std::chrono::seconds(2));

    Bhop_Init();
    Bhop_SetMode(g_cfg.bhop_mode);

    while (g_running) {
        if (g_mem.is_valid()) {
            const uintptr_t local_pawn = g_mem.read<uintptr_t>(
                g_mem.client_dll + offsets::dwLocalPlayerPawn);
            if (local_pawn)
                g_local_team =
                    g_mem.read<uint8_t>(local_pawn + offsets::m_iTeamNum);

            g_esp.update_world(g_mem, g_mem.client_dll);
        }
        // 8 ms (~125 Hz): the game only writes positions at 64 Hz and the
        // render thread interpolates, so faster sampling buys nothing.
        wait_until(std::chrono::steady_clock::now() + std::chrono::milliseconds(8));
    }
}

static ImU32 col_of(const ImVec4& c) { return ImGui::ColorConvertFloat4ToU32(c); }

void render_esp(ImDrawList* dl) {
    if (!g_mem.is_valid()) return;

    const std::vector<PlayerESP> players =
        g_esp.project(g_mem, g_mem.client_dll, g_screen_w, g_screen_h);

    const float lh = ImGui::GetTextLineHeight();

    for (const auto& p : players) {
        const bool is_teammate = (g_local_team != 0 && p.team == g_local_team);
        const bool is_enemy    = !is_teammate;

        if (is_enemy    && !g_cfg.esp_show_enemies) continue;
        if (is_teammate && !g_cfg.esp_show_team)    continue;

        const ImVec4 team_col = is_enemy ? g_cfg.color_enemy : g_cfg.color_team;
        auto pick = [&](const ImVec4& own) -> ImU32 {
            return col_of(g_cfg.team_colors ? team_col : own);
        };

        const float cx  = p.screen_head.x;
        const float top = p.screen_top.y;
        const float bot = p.screen_feet.y;
        const float bh  = p.box_h;
        const float bw  = p.box_w;

        if (cx + bw / 2.0f < 0 || cx - bw / 2.0f > g_screen_w) continue;
        if (bot < 0 || top > g_screen_h) continue;

        // Text spacing scales with the box and is CLAMPED, so labels stay just
        // outside the box at every distance. A fixed pixel offset put the name
        // a whole body-height above distant players, because at range the box
        // is only a few pixels tall while the offset stayed 30 px.
        float gap = bh * 0.06f;
        if (gap < 2.0f) gap = 2.0f;
        if (gap > 8.0f) gap = 8.0f;

        if (g_cfg.esp_boxes) {
            dl->AddRect({ cx - bw / 2.0f, top }, { cx + bw / 2.0f, bot },
                        pick(g_cfg.color_box), 0.0f, 0, g_cfg.box_thickness);
        }

        if (g_cfg.esp_skeleton && p.has_bones) {
            const ImU32 sk = pick(g_cfg.color_skel);
            for (const auto& link : kSkeleton) {
                const int a = link[0], b = link[1];
                if (!p.bone_ok[a] || !p.bone_ok[b]) continue;
                dl->AddLine({ p.bones[a].x, p.bones[a].y },
                            { p.bones[b].x, p.bones[b].y },
                            sk, g_cfg.box_thickness);
            }
        }

        if (g_cfg.esp_head_dot) {
            float r = bh * 0.035f;
            if (r < 2.5f) r = 2.5f;
            if (r > 6.0f) r = 6.0f;
            dl->AddCircleFilled({ p.screen_head.x, p.screen_head.y }, r,
                                pick(g_cfg.color_head), 16);
        }

        if (g_cfg.esp_health) {
            const float bar_h = bh * (p.health / 100.0f);
            const float bar_x = cx - bw / 2.0f - 6.0f;
            const ImU32 hp = IM_COL32((int)(255 * (1.0f - p.health / 100.0f)),
                                      (int)(255 * (p.health / 100.0f)), 0, 255);
            dl->AddRectFilled({ bar_x, bot - bar_h }, { bar_x + 3.0f, bot }, hp);
            dl->AddRect({ bar_x, top }, { bar_x + 3.0f, bot },
                        IM_COL32(0, 0, 0, 180));
        }

        // Name: bottom edge sits `gap` above the box top.
        if (g_cfg.esp_name && p.name[0]) {
            const ImVec2 sz = ImGui::CalcTextSize(p.name);
            dl->AddText({ cx - sz.x * 0.5f, top - gap - lh },
                        pick(g_cfg.color_name), p.name);
        }

        // Weapon: directly under the box.
        if (g_cfg.esp_weapon && p.weapon[0]) {
            const ImVec2 sz = ImGui::CalcTextSize(p.weapon);
            dl->AddText({ cx - sz.x * 0.5f, bot + gap },
                        pick(g_cfg.color_weapon), p.weapon);
        }

        // Bomb carrier: its own line, so it is never confused with the weapon.
        if (g_cfg.esp_bomb && p.has_bomb) {
            const char* tag = "C4 CARRIER";
            const ImVec2 sz = ImGui::CalcTextSize(tag);
            const float y = bot + gap + (g_cfg.esp_weapon && p.weapon[0]
                                             ? lh + gap : 0.0f);
            dl->AddText({ cx - sz.x * 0.5f, y },
                        col_of(g_cfg.color_carrier), tag);
        }

        if (g_cfg.esp_distance) {
            char buf[24];
            std::snprintf(buf, sizeof(buf), "%.0fm", p.distance);
            dl->AddText({ cx + bw / 2.0f + 4.0f, top },
                        col_of(g_cfg.color_dist), buf);
        }
    }

    if (g_cfg.esp_bomb) {
        const BombESP b =
            g_esp.project_bomb(g_mem, g_mem.client_dll, g_screen_w, g_screen_h);
        if (b.active) {
            const ImU32 c = col_of(g_cfg.color_bomb);
            float h = std::fabs(b.screen_top.y - b.screen.y);
            if (h < 6.0f) h = 6.0f;

            dl->AddRect({ b.screen.x - h / 2.0f, b.screen_top.y },
                        { b.screen.x + h / 2.0f, b.screen.y }, c, 0.0f, 0,
                        g_cfg.box_thickness);
            dl->AddText({ b.screen.x - 7.0f, b.screen_top.y - 16.0f }, c, "C4");

            char buf[24];
            std::snprintf(buf, sizeof(buf), "%.0fm", b.distance);
            dl->AddText({ b.screen.x - 14.0f, b.screen.y + 2.0f }, c, buf);
        }
    }
}

static void color_row(const char* id, const char* label, ImVec4* c) {
    ImGui::ColorEdit4(id, &c->x,
        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel);
    ImGui::SameLine();
    ImGui::Text("%s", label);
}

// ── tabs ─────────────────────────────────────────────────────────────────

static void tab_esp() {
    const float col_w = ImGui::GetContentRegionAvail().x / 2.0f;
    ImGui::Columns(2, nullptr, false);
    ImGui::SetColumnWidth(0, col_w);

    ImGui::TextDisabled("[ Visuals ]");
    ImGui::Checkbox("Boxes",      &g_cfg.esp_boxes);
    ImGui::Checkbox("Skeleton",   &g_cfg.esp_skeleton);
    ImGui::Checkbox("Head dot",   &g_cfg.esp_head_dot);
    ImGui::Checkbox("Name",       &g_cfg.esp_name);
    ImGui::Checkbox("Weapon",     &g_cfg.esp_weapon);
    ImGui::Checkbox("Health bar", &g_cfg.esp_health);
    ImGui::Checkbox("Distance",   &g_cfg.esp_distance);
    ImGui::Checkbox("Bomb (C4)",  &g_cfg.esp_bomb);

    ImGui::Spacing();
    ImGui::Checkbox("Show enemies",   &g_cfg.esp_show_enemies);
    ImGui::Checkbox("Show teammates", &g_cfg.esp_show_team);

    ImGui::NextColumn();

    ImGui::TextDisabled("[ Style ]");
    ImGui::SetNextItemWidth(col_w - 16.0f);
    ImGui::SliderFloat("##thick", &g_cfg.box_thickness, 0.5f, 3.0f,
                       "thickness %.1f px");

    ImGui::Spacing();
    ImGui::TextDisabled("[ Smoothing ]");
    ImGui::SetNextItemWidth(col_w - 16.0f);
    ImGui::SliderFloat("##interp", &g_esp.interp_delay_ms, 0.0f, 90.0f, "%.0f ms");
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Snapshot interpolation delay.\n"
                          "Higher = smoother, a touch more latency.\n"
                          "30-45 ms is the sweet spot; 0 disables it.");

    ImGui::Columns(1);
}

static void tab_misc() {
    ImGui::TextDisabled("[ Bhop ]");
    ImGui::TextDisabled("HOLD SPACE to bhop - no auto-jump");

    if (ImGui::RadioButton("Off", g_cfg.bhop_mode == BHOP_OFF)) {
        g_cfg.bhop_mode = BHOP_OFF;   Bhop_SetMode(BHOP_OFF);
    }
    if (ImGui::RadioButton("Inject keys  -  NO writes to CS2",
                           g_cfg.bhop_mode == BHOP_INJECT)) {
        g_cfg.bhop_mode = BHOP_INJECT; Bhop_SetMode(BHOP_INJECT);
    }
    if (ImGui::RadioButton("Write jump button  -  WRITES to CS2",
                           g_cfg.bhop_mode == BHOP_MEMORY)) {
        g_cfg.bhop_mode = BHOP_MEMORY; Bhop_SetMode(BHOP_MEMORY);
    }

    if (g_cfg.bhop_mode == BHOP_MEMORY)
        ImGui::TextColored({ 1.0f, 0.6f, 0.1f, 1.0f },
                           "this mode writes 2 int32s to cs2.exe per jump");
    else if (g_cfg.bhop_mode == BHOP_INJECT)
        ImGui::TextDisabled("no memory writes; timing has OS jitter");

    const BhopDebug bd = Bhop_GetDebug();

    if (g_cfg.bhop_mode == BHOP_MEMORY) {
        ImGui::Text("button: 0x%06llX  %s",
            (unsigned long long)bd.offset,
            bd.locked ? "[locked]" : (bd.scanning ? "[scanning]" : "[idle]"));
        if (!bd.locked)
            ImGui::TextDisabled("hold SPACE to confirm the address");
    }

    ImGui::Text("ground: %s   focused: %s   space: %s",
        bd.on_ground ? "YES" : "no", bd.focused ? "yes" : "NO",
        bd.space_held ? "held" : "-");
    ImGui::TextDisabled("press edges: %d   %s   %s",
        bd.presses, bd.driving ? "[driving]" : "[idle]",
        bd.suppressing ? "[space swallowed]" : "");
    if (!bd.hook_ok)
        ImGui::TextColored({ 1.0f, 0.5f, 0.0f, 1.0f },
                           "key hook failed - space gate may misbehave");

    ImGui::Separator();
    ImGui::TextDisabled("[ Frame rate ]");
    ImGui::Text("panel refresh: %d Hz", g_detected_hz);
    ImGui::Checkbox("Limit to refresh rate", &g_limit_fps);
    ImGui::SetNextItemWidth(180.0f);
    ImGui::SliderInt("##cap", &g_fps_override, 0, 500,
        g_fps_override ? "cap %d fps" : "cap auto (refresh)");
    ImGui::Checkbox("V-Sync", &g_vsync);

    ImGui::Separator();
    ImGui::TextDisabled("entities: %d slots", g_esp.diag_slots);
    ImGui::TextDisabled("weapon defIdx chain: %s (mine=%d)",
                        g_esp.diag_defidx ? "ok" : "not detected",
                        g_esp.diag_defidx);
    if (g_esp.diag_carrier)
        ImGui::TextDisabled("C4 owner: 0x%llX",
                            (unsigned long long)g_esp.diag_carrier);
    else
        ImGui::TextDisabled("C4 owner: not found");
}

static void tab_colors() {
    ImGui::Checkbox("Use team colours", &g_cfg.team_colors);
    ImGui::Separator();

    const float col_w = ImGui::GetContentRegionAvail().x / 2.0f;
    ImGui::Columns(2, nullptr, false);
    ImGui::SetColumnWidth(0, col_w);

    color_row("##ce", "Enemy",        &g_cfg.color_enemy);
    color_row("##ct", "Team",         &g_cfg.color_team);
    color_row("##cb", "Boxes",        &g_cfg.color_box);
    color_row("##cs", "Skeleton",     &g_cfg.color_skel);
    color_row("##ch", "Head dot",     &g_cfg.color_head);

    ImGui::NextColumn();

    color_row("##cn", "Name",         &g_cfg.color_name);
    color_row("##cw", "Weapon",       &g_cfg.color_weapon);
    color_row("##cd", "Distance",     &g_cfg.color_dist);
    color_row("##cc", "Bomb",         &g_cfg.color_bomb);
    color_row("##cr", "Bomb carrier", &g_cfg.color_carrier);

    ImGui::Columns(1);
}

void render_menu() {
    ImGui::SetNextWindowSize({ 580.0f, 460.0f }, ImGuiCond_Once);
    ImGui::SetNextWindowSizeConstraints({ 440.0f, 280.0f }, { 900.0f, 760.0f });
    ImGui::SetNextWindowPos({ 20.0f, 20.0f }, ImGuiCond_Once);
    ImGui::Begin("Orbital", nullptr);

    if (!g_mem.is_valid()) {
        ImGui::TextColored({ 1.0f, 0.5f, 0.0f, 1.0f }, "[ waiting for CS2 ]");
    } else {
        ImGui::TextColored({ 0.0f, 0.7f, 0.0f, 1.0f },
            "[ attached  %dx%d  %d Hz  %d players ]",
            g_screen_w, g_screen_h, g_detected_hz, g_esp.players_alive);
    }

    ImGui::Separator();

    if (ImGui::BeginTabBar("##orbital_tabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem("ESP"))    { tab_esp();    ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("MISC"))   { tab_misc();   ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("COLORS")) { tab_colors(); ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }

    ImGui::Separator();

    const float btn_w =
        (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f)
        / 3.0f;

    if (ImGui::Button("[Reset]", { btn_w, 0 })) {
        g_cfg = Config{};
        g_esp.interp_delay_ms = 35.0f;
        Bhop_SetMode(g_cfg.bhop_mode);
    }
    ImGui::SameLine();
    if (ImGui::Button("[Rescan bhop]", { btn_w, 0 })) Bhop_Rescan();
    ImGui::SameLine();
    if (ImGui::Button("[Exit]", { btn_w, 0 })) g_running = false;

    ImGui::TextDisabled("INSERT - menu    F9 - exit");

    ImGui::End();
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    Overlay::enable_dpi_awareness();

    while (!find_cs2_window())
        std::this_thread::sleep_for(std::chrono::milliseconds(500));

    g_detected_hz = query_refresh_rate(g_cs2_hwnd);

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

        if (g_cs2_hwnd && IsWindow(g_cs2_hwnd)) {
            RECT r{};
            if (GetClientRect(g_cs2_hwnd, &r) && r.right > 0 && r.bottom > 0) {
                g_screen_w = r.right;
                g_screen_h = r.bottom;
            }
            overlay.sync_to_game();
        }

        overlay.begin_frame();
        ImDrawList* dl = ImGui::GetBackgroundDrawList();
        render_esp(dl);
        if (g_menu_open) render_menu();
        overlay.end_frame();

        static auto next_hz = std::chrono::steady_clock::now();
        const auto now_tp = std::chrono::steady_clock::now();
        if (now_tp >= next_hz) {
            const int hz = query_refresh_rate(g_cs2_hwnd);
            if (hz) g_detected_hz = hz;
            next_hz = now_tp + std::chrono::seconds(2);
        }

        if (!g_vsync) {
            int cap = 0;
            if (g_limit_fps)
                cap = (g_fps_override > 0) ? g_fps_override : g_detected_hz;

            if (cap > 0) {
                static auto next = std::chrono::steady_clock::now();
                next += std::chrono::microseconds(1000000 / cap);
                const auto now = std::chrono::steady_clock::now();
                if (next < now) next = now;
                wait_until(next);
            }
        }
    }

    g_running = false;
    mem_t.join();
    Bhop_Shutdown();
    overlay.cleanup();
    g_mem.detach();
    return 0;
}
