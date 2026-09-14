// --- src/main.cpp ---
#include <Windows.h>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>

#include <imgui.h>

#include "memory.h"
#include "esp.h"
#include "overlay.h"
#include "offsets.h"
#include "bhop.h"
#include "aim.h"
#include "movement.h"

static ESP g_esp;
static bool g_running = true;
bool g_menu_open = false;
bool g_vsync = false;
HWND g_cs2_hwnd = nullptr;

// ── logging ──────────────────────────────────────────────────────────────
static FILE* g_log = nullptr;

static void open_log() {
    if (g_log) return;

    wchar_t path[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, path, MAX_PATH)) {
        wchar_t* slash = wcsrchr(path, L'\\');
        if (slash) {
            *(slash + 1) = L'\0';
            wcscat_s(path, MAX_PATH, L"orbital_log.txt");
            _wfopen_s(&g_log, path, L"w");
        }
    }
    if (!g_log) _wfopen_s(&g_log, L"C:\\orbital_log.txt", L"w");
}

void OrbitalLog(const char* fmt, ...) {
    open_log();
    if (!g_log) return;
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_log, fmt, ap);
    va_end(ap);
    fputc('\n', g_log);
    fflush(g_log);
}

static void sidecar_path(wchar_t* out, size_t cap, const wchar_t* name) {
    out[0] = L'\0';
    wchar_t exe[MAX_PATH]{};
    if (GetModuleFileNameW(nullptr, exe, MAX_PATH)) {
        wchar_t* slash = wcsrchr(exe, L'\\');
        if (slash) {
            *(slash + 1) = L'\0';
            swprintf_s(out, cap, L"%s%s", exe, name);
        }
    }
    if (!out[0]) swprintf_s(out, cap, L"C:\\%s", name);
}

// ══ GLOBALS THAT THE CONFIG CODE USES MUST COME FIRST ════════════════════
// The previous version inserted load_config() ABOVE these declarations, so
// g_screen_w and friends did not exist where the config code referenced them.
static int  g_screen_w = 1920;
static int  g_screen_h = 1080;
static int  g_local_team = 0;
static int  g_detected_hz = 0;
static bool g_limit_fps = true;
static int  g_fps_override = 0;

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
    bool  esp_show_enemies = true;
    bool  esp_show_team    = false;
    float box_thickness = 0.5f;

    // ESP colours.
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

    // Menu skin.
    ImVec4 menu_title  = { 0.00f, 0.00f, 0.70f, 1.00f };
    ImVec4 menu_button = { 0.88f, 0.80f, 0.55f, 1.00f };
    ImVec4 menu_slider = { 0.55f, 0.55f, 0.55f, 1.00f };

    // Feature toggles.
    bool  bh_enabled  = false;
    bool  aim_enabled = false;
    bool  aim_fire    = true;
    int   aim_key     = 0;
    bool  jb_enabled  = false;
} g_cfg;

// ── config file ──────────────────────────────────────────────────────────
static void save_config() {
    wchar_t path[MAX_PATH]{};
    sidecar_path(path, MAX_PATH, L"orbital.cfg");

    FILE* f = nullptr;
    _wfopen_s(&f, path, L"w");
    if (!f) { OrbitalLog("config save FAILED"); return; }

#define W_B(field) fprintf(f, #field "=%d\n", g_cfg.field ? 1 : 0)
#define W_I(field) fprintf(f, #field "=%d\n", g_cfg.field)
#define W_F(field) fprintf(f, #field "=%.4f\n", g_cfg.field)
#define W_C(field) fprintf(f, #field "=%.4f %.4f %.4f %.4f\n", \
                           g_cfg.field.x, g_cfg.field.y, \
                           g_cfg.field.z, g_cfg.field.w)

    W_B(esp_boxes);        W_B(esp_skeleton);  W_B(esp_head_dot);
    W_B(esp_name);         W_B(esp_weapon);    W_B(esp_health);
    W_B(esp_distance);     W_B(esp_bomb);
    W_B(esp_show_enemies); W_B(esp_show_team);
    W_F(box_thickness);

    W_B(team_colors);
    W_C(color_enemy);  W_C(color_team);   W_C(color_box);
    W_C(color_skel);   W_C(color_head);   W_C(color_name);
    W_C(color_weapon); W_C(color_dist);   W_C(color_bomb);
    W_C(color_carrier);

    W_C(menu_title);   W_C(menu_button);  W_C(menu_slider);

    W_B(bh_enabled);
    W_B(aim_enabled);  W_B(aim_fire);     W_I(aim_key);
    W_B(jb_enabled);

#undef W_B
#undef W_I
#undef W_F
#undef W_C

    fclose(f);
    OrbitalLog("config saved");
}

static void load_config() {
    wchar_t path[MAX_PATH]{};
    sidecar_path(path, MAX_PATH, L"orbital.cfg");

    FILE* f = nullptr;
    _wfopen_s(&f, path, L"r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* key = line;
        const char* val = eq + 1;

        auto as_bool  = [&](bool& field)  { field = (atoi(val) != 0); };
        auto as_int   = [&](int& field)   { field = atoi(val); };
        auto as_float = [&](float& field) { field = static_cast<float>(atof(val)); };
        auto as_col   = [&](ImVec4& field) {
            float r = 0, g = 0, bl = 0, a = 1;
            sscanf_s(val, "%f %f %f %f", &r, &g, &bl, &a);
            field = ImVec4(r, g, bl, a);
        };

        if      (!strcmp(key, "esp_boxes"))        as_bool(g_cfg.esp_boxes);
        else if (!strcmp(key, "esp_skeleton"))     as_bool(g_cfg.esp_skeleton);
        else if (!strcmp(key, "esp_head_dot"))     as_bool(g_cfg.esp_head_dot);
        else if (!strcmp(key, "esp_name"))         as_bool(g_cfg.esp_name);
        else if (!strcmp(key, "esp_weapon"))       as_bool(g_cfg.esp_weapon);
        else if (!strcmp(key, "esp_health"))       as_bool(g_cfg.esp_health);
        else if (!strcmp(key, "esp_distance"))     as_bool(g_cfg.esp_distance);
        else if (!strcmp(key, "esp_bomb"))         as_bool(g_cfg.esp_bomb);
        else if (!strcmp(key, "esp_show_enemies")) as_bool(g_cfg.esp_show_enemies);
        else if (!strcmp(key, "esp_show_team"))    as_bool(g_cfg.esp_show_team);
        else if (!strcmp(key, "box_thickness"))    as_float(g_cfg.box_thickness);
        else if (!strcmp(key, "team_colors"))      as_bool(g_cfg.team_colors);
        else if (!strcmp(key, "color_enemy"))      as_col(g_cfg.color_enemy);
        else if (!strcmp(key, "color_team"))       as_col(g_cfg.color_team);
        else if (!strcmp(key, "color_box"))        as_col(g_cfg.color_box);
        else if (!strcmp(key, "color_skel"))       as_col(g_cfg.color_skel);
        else if (!strcmp(key, "color_head"))       as_col(g_cfg.color_head);
        else if (!strcmp(key, "color_name"))       as_col(g_cfg.color_name);
        else if (!strcmp(key, "color_weapon"))     as_col(g_cfg.color_weapon);
        else if (!strcmp(key, "color_dist"))       as_col(g_cfg.color_dist);
        else if (!strcmp(key, "color_bomb"))       as_col(g_cfg.color_bomb);
        else if (!strcmp(key, "color_carrier"))    as_col(g_cfg.color_carrier);
        else if (!strcmp(key, "menu_title"))       as_col(g_cfg.menu_title);
        else if (!strcmp(key, "menu_button"))      as_col(g_cfg.menu_button);
        else if (!strcmp(key, "menu_slider"))      as_col(g_cfg.menu_slider);
        else if (!strcmp(key, "bh_enabled"))       as_bool(g_cfg.bh_enabled);
        else if (!strcmp(key, "aim_enabled"))      as_bool(g_cfg.aim_enabled);
        else if (!strcmp(key, "aim_fire"))         as_bool(g_cfg.aim_fire);
        else if (!strcmp(key, "aim_key"))          as_int(g_cfg.aim_key);
        else if (!strcmp(key, "jb_enabled"))       as_bool(g_cfg.jb_enabled);
    }
    fclose(f);
    OrbitalLog("config loaded");
}

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

// ── game window lookup ───────────────────────────────────────────────────
struct WindowCand {
    HWND hwnd;
    int  w, h;
    bool titled;
};

static BOOL CALLBACK enum_windows_proc(HWND h, LPARAM lp) {
    auto* list = reinterpret_cast<std::vector<WindowCand>*>(lp);
    if (!list) return TRUE;

    if (!IsWindowVisible(h)) return TRUE;
    if (IsIconic(h)) return TRUE;

    wchar_t cls[64]{};
    if (!GetClassNameW(h, cls, 63)) return TRUE;
    if (lstrcmpW(cls, L"SDL_app") != 0) return TRUE;

    RECT r{};
    if (!GetClientRect(h, &r)) return TRUE;
    const int w  = r.right - r.left;
    const int hh = r.bottom - r.top;
    if (w <= 0 || hh <= 0) return TRUE;

    wchar_t title[128]{};
    GetWindowTextW(h, title, 127);
    const bool titled = (wcsstr(title, L"Counter-Strike") != nullptr);

    list->push_back(WindowCand{ h, w, hh, titled });
    OrbitalLog("  sdl window 0x%p client %dx%d titled=%d", h, w, hh,
               titled ? 1 : 0);
    return TRUE;
}

static HWND locate_game_window() {
    std::vector<WindowCand> cands;
    EnumWindows(enum_windows_proc, reinterpret_cast<LPARAM>(&cands));

    HWND best = nullptr;
    long long best_score = -1;
    for (const WindowCand& c : cands) {
        long long score = static_cast<long long>(c.w) * c.h;
        if (!c.titled)              score /= 4;
        if (c.w < 640 || c.h < 480) score /= 4;
        if (score > best_score) { best_score = score; best = c.hwnd; }
    }
    if (best) return best;

    HWND h = FindWindowA("SDL_app", nullptr);
    return (h && IsWindow(h)) ? h : nullptr;
}

static bool refresh_game_window() {
    if (g_cs2_hwnd && IsWindow(g_cs2_hwnd)) {
        RECT r{};
        if (GetClientRect(g_cs2_hwnd, &r) && r.right > 0 && r.bottom > 0) {
            g_screen_w = r.right;
            g_screen_h = r.bottom;
            return true;
        }
    }

    const HWND h = locate_game_window();
    if (!h) return false;

    RECT r{};
    if (!GetClientRect(h, &r) || r.right <= 0 || r.bottom <= 0) return false;

    g_cs2_hwnd = h;
    g_screen_w = r.right;
    g_screen_h = r.bottom;
    OrbitalLog("game window chosen: 0x%p client %dx%d", h, g_screen_w, g_screen_h);
    return true;
}

static void use_monitor_size() {
    const int w = GetSystemMetrics(SM_CXSCREEN);
    const int h = GetSystemMetrics(SM_CYSCREEN);
    if (w > 0 && h > 0) { g_screen_w = w; g_screen_h = h; }
}

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

static void apply_feature_config() {
    Bhop_SetEnabled(g_cfg.bh_enabled);

    Aim_SetEnabled(g_cfg.aim_enabled);
    Aim_SetFire(g_cfg.aim_fire);
    Aim_SetKey(g_cfg.aim_key);

    Movement_SetJumpbug(g_cfg.jb_enabled);
}

void memory_thread() {
    while (g_running && !g_mem.attach(L"cs2.exe"))
        wait_until(std::chrono::steady_clock::now() + std::chrono::seconds(2));

    OrbitalLog("memory attach: %s", g_mem.is_valid() ? "ok" : "FAILED");

    Bhop_Init();
    Aim_Init();
    Movement_Init();
    apply_feature_config();

    while (g_running) {
        if (g_mem.is_valid()) {
            const uintptr_t local_pawn = g_mem.read<uintptr_t>(
                g_mem.client_dll + offsets::dwLocalPlayerPawn);
            if (local_pawn) {
                g_local_team =
                    g_mem.read<uint8_t>(local_pawn + offsets::m_iTeamNum);
            }

            g_esp.update_world(g_mem, g_mem.client_dll);
        }
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

        if (g_cfg.esp_name && p.name[0]) {
            const ImVec2 sz = ImGui::CalcTextSize(p.name);
            dl->AddText({ cx - sz.x * 0.5f, top - gap - lh },
                        pick(g_cfg.color_name), p.name);
        }

        if (g_cfg.esp_weapon && p.weapon[0]) {
            const ImVec2 sz = ImGui::CalcTextSize(p.weapon);
            dl->AddText({ cx - sz.x * 0.5f, bot + gap },
                        pick(g_cfg.color_weapon), p.weapon);
        }

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

static void tab_aim() {
    ImGui::TextDisabled("[ Triggerbot ]");
    ImGui::TextDisabled("detection: read-only     firing: injects a click");
    ImGui::Separator();

    if (ImGui::Checkbox("Triggerbot", &g_cfg.aim_enabled))
        Aim_SetEnabled(g_cfg.aim_enabled);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Fires when the crosshair is within range of an enemy\n"
                          "bone. Uses the NEWEST position, not the smoothed one,\n"
                          "so it does not fire behind a moving target.");

    if (ImGui::Checkbox("Firing (injects left click)", &g_cfg.aim_fire))
        Aim_SetFire(g_cfg.aim_fire);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Unticked: the trigger still detects and shows the\n"
                          "indicator, but never clicks. That makes the feature\n"
                          "read-only.");

    static const char* const kKeyNames[] = {
        "always on", "MOUSE5", "MOUSE4", "ALT", "SHIFT", "CTRL", "CAPS"
    };
    static const int kKeyVks[] = {
        0, VK_XBUTTON2, VK_XBUTTON1, VK_MENU, VK_SHIFT, VK_CONTROL, VK_CAPITAL
    };
    constexpr int kKeyCount = IM_ARRAYSIZE(kKeyNames);

    int cur = 0;
    for (int i = 0; i < kKeyCount; ++i)
        if (kKeyVks[i] == g_cfg.aim_key) cur = i;

    ImGui::SetNextItemWidth(200.0f);
    if (ImGui::Combo("##aimkey", &cur, kKeyNames, kKeyCount)) {
        g_cfg.aim_key = kKeyVks[cur];
        Aim_SetKey(g_cfg.aim_key);
    }

    const AimDebug ad = Aim_GetDebug();
    ImGui::Separator();
    if (ad.on_target)
        ImGui::TextColored({ 0.1f, 0.6f, 0.1f, 1.0f },
                           "on target: %s  (%.0f px)", ad.target_bone,
                           ad.target_dist);
    else
        ImGui::TextDisabled("on target: no");
    ImGui::TextDisabled("firing: %s", ad.firing ? "yes" : "no");
}

static void tab_movement() {
    ImGui::TextDisabled("[ Movement ]");
    ImGui::TextDisabled("injects CTRL; nothing is written to CS2");
    ImGui::Separator();

    if (ImGui::Checkbox("Jumpbug", &g_cfg.jb_enabled))
        Movement_SetJumpbug(g_cfg.jb_enabled);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Crouches just before you land, then releases after\n"
                          "touchdown. Cancels fall damage reliably, and gains\n"
                          "height on the frames the crouch lines up with the\n"
                          "landing. No key needed - it arms itself while\n"
                          "airborne and falling.");

    const MovementDebug md = Movement_GetDebug();
    ImGui::Text("ground: %s   crouching: %s",
        md.on_ground ? "YES" : "no", md.crouching ? "yes" : "no");
    ImGui::TextDisabled("vz %.0f   tti %.1f ms   jumpbugs %d",
        md.vz, md.tti, md.jumpbugs);

    ImGui::Separator();
    ImGui::TextDisabled("[ Bhop ]");
    ImGui::TextDisabled("HOLD SPACE - no auto-jump, no keybind");
    if (ImGui::Checkbox("Bhop (space)", &g_cfg.bh_enabled))
        Bhop_SetEnabled(g_cfg.bh_enabled);

    const BhopDebug bd = Bhop_GetDebug();
    ImGui::Text("ground: %s   focused: %s   space: %s",
        bd.on_ground ? "YES" : "no", bd.focused ? "yes" : "NO",
        bd.space_held ? "held" : "-");

    ImGui::Separator();
    ImGui::TextDisabled("[ Frame rate ]");
    ImGui::Text("panel refresh: %d Hz", g_detected_hz);
    ImGui::Checkbox("Limit to refresh rate", &g_limit_fps);
    ImGui::SetNextItemWidth(180.0f);
    ImGui::SliderInt("##cap", &g_fps_override, 0, 500,
        g_fps_override ? "cap %d fps" : "cap auto (refresh)");
    ImGui::Checkbox("V-Sync", &g_vsync);
}

static void tab_radar() {
    ImGui::TextDisabled("[ Radar ]");
    ImGui::Separator();
    ImGui::Spacing();
    ImGui::TextDisabled("not implemented yet");
    ImGui::Spacing();
    ImGui::TextDisabled("Reserved for the radar module.");
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

    ImGui::Separator();
    ImGui::TextDisabled("[ Menu ]");
    ImGui::Columns(2, nullptr, false);
    ImGui::SetColumnWidth(0, col_w);

    color_row("##mt", "Title bar",    &g_cfg.menu_title);
    color_row("##mb", "Buttons",      &g_cfg.menu_button);

    ImGui::NextColumn();
    color_row("##ms", "Slider bg",    &g_cfg.menu_slider);

    ImGui::Columns(1);
}

static void tab_misc() {
    ImGui::TextDisabled("[ Config ]");
    ImGui::Separator();
    ImGui::TextDisabled("saved next to the exe as orbital.cfg");
    ImGui::TextDisabled("also saved automatically on exit");
    ImGui::Spacing();
    ImGui::TextDisabled("[ game window ]");
    ImGui::TextDisabled("found: %s",
        (g_cs2_hwnd && IsWindow(g_cs2_hwnd)) ? "yes" : "NO");
    ImGui::TextDisabled("viewport: %dx%d", g_screen_w, g_screen_h);
    ImGui::TextDisabled("refresh: %d Hz", g_detected_hz);
}

void render_menu() {
    {
        ImGuiStyle& st = ImGui::GetStyle();
        st.Colors[ImGuiCol_TitleBg]       = g_cfg.menu_title;
        st.Colors[ImGuiCol_TitleBgActive] = g_cfg.menu_title;
        st.Colors[ImGuiCol_Button]        = g_cfg.menu_button;
        st.Colors[ImGuiCol_SliderGrab]    = g_cfg.menu_slider;
    }

    ImGui::SetNextWindowSize({ 600.0f, 540.0f }, ImGuiCond_Once);
    ImGui::SetNextWindowSizeConstraints({ 500.0f, 320.0f }, { 1000.0f, 900.0f });
    ImGui::SetNextWindowPos({ 20.0f, 20.0f }, ImGuiCond_Once);
    ImGui::Begin("Orbital - Mars", nullptr);

    if (!g_mem.is_valid()) {
        ImGui::TextColored({ 1.0f, 0.5f, 0.0f, 1.0f }, "[ waiting for CS2 ]");
    } else {
        ImGui::TextColored({ 0.0f, 0.7f, 0.0f, 1.0f },
            "[ attached  %dx%d  %d Hz  %d players ]",
            g_screen_w, g_screen_h, g_detected_hz, g_esp.players_alive);
    }

    ImGui::Separator();

    if (ImGui::BeginTabBar("##orbital_tabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem("ESP"))      { tab_esp();      ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("AIM"))      { tab_aim();      ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("MOVEMENT")) { tab_movement(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("RADAR"))    { tab_radar();    ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("COLORS"))   { tab_colors();   ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("MISC"))     { tab_misc();     ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }

    ImGui::Separator();

    const float btn_w3 =
        (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x * 2.0f)
        / 3.0f;

    if (ImGui::Button("[Save]", { btn_w3, 0 })) save_config();
    ImGui::SameLine();
    if (ImGui::Button("[Reset]", { btn_w3, 0 })) {
        g_cfg = Config{};
        g_esp.interp_delay_ms = 35.0f;
        apply_feature_config();
    }
    ImGui::SameLine();
    if (ImGui::Button("[Exit]", { btn_w3, 0 })) g_running = false;

    ImGui::TextDisabled("INSERT - menu (in-game only)    F9 - exit");

    ImGui::End();
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    OrbitalLog("=== orbital start ===");

    Overlay::enable_dpi_awareness();
    OrbitalLog("dpi awareness set");

    load_config();

    bool have_game = false;
    for (int i = 0; i < 20 && !have_game; ++i) {
        have_game = refresh_game_window();
        if (!have_game)
            std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (!have_game) {
        OrbitalLog("game window NOT found after 5s - using monitor size");
        use_monitor_size();
    } else {
        OrbitalLog("game window ok (%dx%d)", g_screen_w, g_screen_h);
    }

    g_detected_hz = query_refresh_rate(g_cs2_hwnd);
    OrbitalLog("refresh rate: %d Hz", g_detected_hz);

    Overlay overlay;
    if (!overlay.create(g_screen_w, g_screen_h)) {
        OrbitalLog("overlay.create FAILED - exiting");
        return 1;
    }
    OrbitalLog("overlay created (%dx%d)", g_screen_w, g_screen_h);

    std::thread mem_t(memory_thread);

    OrbitalLog("entering message loop");

    MSG msg{};
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) g_running = false;
        }

        // INSERT only acts while CS2 is the foreground window, and losing focus
        // closes the menu, so it can never be left over another application.
        const bool cs2_active =
            (g_cs2_hwnd && IsWindow(g_cs2_hwnd) &&
             GetForegroundWindow() == g_cs2_hwnd);

        if (!cs2_active && g_menu_open) g_menu_open = false;

        if (cs2_active && (GetAsyncKeyState(VK_INSERT) & 1))
            g_menu_open = !g_menu_open;
        if (GetAsyncKeyState(VK_F9) & 1) g_running = false;

        refresh_game_window();
        overlay.sync_to_game();

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

    OrbitalLog("shutting down");
    save_config();
    g_running = false;
    mem_t.join();
    Bhop_Shutdown();
    Aim_Shutdown();
    Movement_Shutdown();
    overlay.cleanup();
    g_mem.detach();

    if (g_log) { fclose(g_log); g_log = nullptr; }
    return 0;
}
