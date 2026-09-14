// --- src/main.cpp ---
#include <Windows.h>
#include <chrono>
#include <cstdarg>
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

// ── logging ──────────────────────────────────────────────────────────────
// Every init step is logged, because the failure modes here are SILENT: a
// blocked startup loop, a wrongly-chosen window, or a self-hiding overlay all
// look identical from outside. The log says which one it was.
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

    // Bhop -- six independent engines, nothing writes to cs2.exe.
    bool  bh_scroll_si   = false;
    bool  bh_key_edge    = false;
    bool  bh_key_edge_del = true;   // was the most consistent
    bool  bh_scroll_me   = false;
    bool  bh_fps64       = false;
    bool  bh_key_repeat  = false;
    float bh_scroll_ms   = 8.0f;
    float bh_repeat_ms   = 15.625f; // retry cadence
    float bh_delay_ms    = 15.625f; // one tick
    int   bh_inject_key  = VK_F20;
    int   bh_fps_target  = 64;
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
// FindWindowA("SDL_app") returns the FIRST match in Z-order, and CS2 does not
// keep the game window in front of its own detached console. Taking the first
// match landed us on a 192x456 console window. So we enumerate every SDL
// window, log all of them, and pick the best: titled first, then largest client
// area, with small windows penalised so a console can never outrank the game.
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
    const bool titled =
        (wcsstr(title, L"Counter-Strike") != nullptr);

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

// Push the whole bhop config into the module, so the two can never drift apart.
static void apply_bhop_config() {
    Bhop_SetEngine(BHOP_SCROLL_SI,    g_cfg.bh_scroll_si);
    Bhop_SetEngine(BHOP_KEY_EDGE,     g_cfg.bh_key_edge);
    Bhop_SetEngine(BHOP_KEY_EDGE_DEL, g_cfg.bh_key_edge_del);
    Bhop_SetEngine(BHOP_SCROLL_ME,    g_cfg.bh_scroll_me);
    Bhop_SetEngine(BHOP_FPS64,        g_cfg.bh_fps64);
    Bhop_SetEngine(BHOP_KEY_REPEAT,   g_cfg.bh_key_repeat);
    Bhop_SetScrollInterval(g_cfg.bh_scroll_ms);
    Bhop_SetRepeatMs(g_cfg.bh_repeat_ms);
    Bhop_SetDelayMs(g_cfg.bh_delay_ms);
    Bhop_SetInjectKey(g_cfg.bh_inject_key);
    Bhop_SetFpsTarget(g_cfg.bh_fps_target);
}

void memory_thread() {
    while (g_running && !g_mem.attach(L"cs2.exe"))
        wait_until(std::chrono::steady_clock::now() + std::chrono::seconds(2));

    OrbitalLog("memory attach: %s", g_mem.is_valid() ? "ok" : "FAILED");

    Bhop_Init();
    apply_bhop_config();

    while (g_running) {
        if (g_mem.is_valid()) {
            const uintptr_t local_pawn = g_mem.read<uintptr_t>(
                g_mem.client_dll + offsets::dwLocalPlayerPawn);
            if (local_pawn)
                g_local_team =
                    g_mem.read<uint8_t>(local_pawn + offsets::m_iTeamNum);

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

// ── MISC: five independent bhop engines ──────────────────────────────────

static void engine_row(int engine, const char* label, const char* tip,
                       bool* cfg_flag) {
    if (ImGui::Checkbox(label, cfg_flag))
        Bhop_SetEngine(engine, *cfg_flag);
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", tip);

    const BhopDebug d = Bhop_GetDebug();
    ImGui::SameLine(320.0f);
    if (d.active[engine]) {
        ImGui::TextColored({ 0.1f, 0.6f, 0.1f, 1.0f }, "running");
    } else {
        ImGui::TextDisabled("-");
    }
    ImGui::SameLine();

    // The age is the diagnostic that matters: if hops stop while age keeps
    // resetting, we are still injecting and the game is ignoring us. If age
    // climbs, our own logic has stalled.
    if (d.age_ms[engine] < 0)
        ImGui::TextDisabled("%d  never", d.injected[engine]);
    else if (d.age_ms[engine] < 1000)
        ImGui::TextDisabled("%d  %dms ago", d.injected[engine],
                            d.age_ms[engine]);
    else
        ImGui::TextDisabled("%d  stopped %ds ago", d.injected[engine],
                            d.age_ms[engine] / 1000);
}

static void tab_misc() {
    ImGui::TextDisabled("[ Bhop ]  HOLD SPACE - no auto-jump");
    ImGui::TextDisabled("all engines inject input; none write to CS2");
    ImGui::Separator();

    engine_row(BHOP_SCROLL_SI, "1. Scroll - SendInput",
        "Spams mouse-wheel events while the gate is held.\n"
        "Continuous, so it has no state that can latch.\n"
        "Needs the scroll bind in game (see below).",
        &g_cfg.bh_scroll_si);

    engine_row(BHOP_KEY_EDGE, "2. Key pair - while grounded",
        "Injects F20 down+up pairs for as long as the ground flag\n"
        "says you are on the ground. Retries automatically, so a\n"
        "missed hop is corrected on the next attempt instead of\n"
        "ending the chain. Needs the F-key bind.",
        &g_cfg.bh_key_edge);

    engine_row(BHOP_KEY_EDGE_DEL, "3. Key pair - one tick delay",
        "Same as 2, but the first pair of each grounded period waits\n"
        "one client tick (15.625 ms). This was the most consistent of\n"
        "the previous engines, and now it retries instead of stopping.",
        &g_cfg.bh_key_edge_del);

    engine_row(BHOP_SCROLL_ME, "4. Scroll - mouse_event",
        "Wheel spam through the older mouse_event API. Travels a\n"
        "different path through the Windows input stack than\n"
        "SendInput, so if CS2 filters one it may not filter the other.",
        &g_cfg.bh_scroll_me);

    engine_row(BHOP_FPS64, "5. 64 FPS tick-aligned",
        "Types fps_max 64 into the CS2 console, then uses the\n"
        "one-tick-delay pattern. At 64 fps frames align 1:1 with the\n"
        "server tick, so input lands in the tick intended.\n"
        "Disabling this restores fps_max 0.",
        &g_cfg.bh_fps64);

    engine_row(BHOP_KEY_REPEAT, "6. Key pair - always (no ground check)",
        "Ignores the ground flag entirely and injects a pair at a\n"
        "fixed cadence for as long as you hold space. There is no\n"
        "state machine at all, so nothing can latch or stall -- the\n"
        "most failure-proof option. While airborne the presses rely\n"
        "on the game's jump buffering.",
        &g_cfg.bh_key_repeat);

    const BhopDebug bd = Bhop_GetDebug();

    ImGui::Columns(2, nullptr, false);
    ImGui::SetColumnWidth(0, 260.0f);

    ImGui::Spacing();
    ImGui::TextDisabled("[ Tuning ]");
    ImGui::SetNextItemWidth(230.0f);
    if (ImGui::SliderFloat("##scrollms", &g_cfg.bh_scroll_ms, 2.0f, 30.0f,
                           "scroll every %.0f ms"))
        Bhop_SetScrollInterval(g_cfg.bh_scroll_ms);

    ImGui::SetNextItemWidth(230.0f);
    if (ImGui::SliderFloat("##repeatms", &g_cfg.bh_repeat_ms, 4.0f, 40.0f,
                           "retry every %.3f ms"))
        Bhop_SetRepeatMs(g_cfg.bh_repeat_ms);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How often a key engine retries while grounded.\n"
                          "One tick is 15.625 ms. This is the setting that\n"
                          "keeps the bhop from stopping.");

    ImGui::SetNextItemWidth(230.0f);
    if (ImGui::SliderFloat("##delayms", &g_cfg.bh_delay_ms, 0.0f, 40.0f,
                           "first-pair delay %.3f ms"))
        Bhop_SetDelayMs(g_cfg.bh_delay_ms);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Engines 3 and 5 only: how long the FIRST pair of\n"
                          "each grounded period waits. One tick is 15.625 ms.");

    // Plain parallel arrays + the classic string-array Combo overload. The
    // getter-based Combo (bool(*)(void*,int,const char**)) only exists in newer
    // ImGui, so it is deliberately not used here.
    static const char* const kKeyNames[] = {
        "F20", "F19", "F18", "APPS (menu key)", "RIGHT", "SPACE", "ALT"
    };
    static const int kKeyVks[] = {
        VK_F20, VK_F19, VK_F18, VK_APPS, VK_RIGHT, VK_SPACE, VK_MENU
    };
    constexpr int kKeyCount = IM_ARRAYSIZE(kKeyNames);

    int cur = 0;
    for (int i = 0; i < kKeyCount; ++i)
        if (kKeyVks[i] == g_cfg.bh_inject_key) cur = i;

    ImGui::SetNextItemWidth(230.0f);
    if (ImGui::Combo("##key", &cur, kKeyNames, kKeyCount)) {
        g_cfg.bh_inject_key = kKeyVks[cur];
        Bhop_SetInjectKey(g_cfg.bh_inject_key);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Key injected by engines 2, 3 and 5.\n"
                          "Some VK codes are ignored by CS2 - if a key does\n"
                          "nothing, try another. You must bind it in game.");

    ImGui::SetNextItemWidth(230.0f);
    if (ImGui::SliderInt("##fps", &g_cfg.bh_fps_target, 32, 300,
                         "fps_max %d"))
        Bhop_SetFpsTarget(g_cfg.bh_fps_target);

    ImGui::NextColumn();

    ImGui::Spacing();
    ImGui::TextDisabled("[ State ]");
    ImGui::Text("ground: %s   focused: %s   space: %s",
        bd.on_ground ? "YES" : "no", bd.focused ? "yes" : "NO",
        bd.space_held ? "held" : "-");
    ImGui::TextDisabled("signals: %s%s%s",
        (bd.signals & 1) ? "z " : "",
        (bd.signals & 2) ? "flag " : "",
        (bd.signals & 4) ? "hge" : "");
    if (bd.suppressing)
        ImGui::TextDisabled("space swallowed while driving");
    if (!bd.hook_ok)
        ImGui::TextColored({ 1.0f, 0.5f, 0.0f, 1.0f },
                           "key hook failed - gate may misbehave");
    if (bd.fps_cmd_sent)
        ImGui::TextDisabled("fps_max %d sent", bd.fps_target);

    ImGui::Columns(1);

    ImGui::Separator();
    ImGui::TextDisabled("[ in-game binds - paste one line at a time ]");
    if (ImGui::BeginChild("##binds", { 0.0f, 108.0f }, true)) {
        ImGui::TextDisabled("// scroll engines (1 and 4)");
        ImGui::Text("alias +jh \"+jump;+jump\"");
        ImGui::Text("alias -jh \"-jump;-jump;-jump\"");
        ImGui::Text("bind mwheelup +jh");
        ImGui::Text("bind mwheeldown +jh");
        ImGui::TextDisabled("// key engines (2, 3 and 5)");
        ImGui::Text("bind F20 +jump");
    }
    ImGui::EndChild();
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("The doubled +jump/-jump is the desubtick trick:\n"
                          "each wheel input fires the command several times,\n"
                          "so the engine gets more chances to register an edge.\n"
                          "Plain mwheel +jump is unreliable post-July-2025.\n"
                          "If 'bind F20' errors, CS2 cannot bind that key --\n"
                          "pick a different one in the Tuning list.");

    ImGui::Separator();
    ImGui::TextDisabled("[ Frame rate ]");
    ImGui::Text("panel refresh: %d Hz", g_detected_hz);
    ImGui::Checkbox("Limit to refresh rate", &g_limit_fps);
    ImGui::SetNextItemWidth(180.0f);
    ImGui::SliderInt("##cap", &g_fps_override, 0, 500,
        g_fps_override ? "cap %d fps" : "cap auto (refresh)");
    ImGui::Checkbox("V-Sync", &g_vsync);

    ImGui::Separator();
    ImGui::TextDisabled("game window: %s",
        (g_cs2_hwnd && IsWindow(g_cs2_hwnd)) ? "found" : "NOT FOUND");
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
    ImGui::SetNextWindowSize({ 640.0f, 620.0f }, ImGuiCond_Once);
    ImGui::SetNextWindowSizeConstraints({ 520.0f, 340.0f }, { 1000.0f, 900.0f });
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
        (ImGui::GetContentRegionAvail().x - ImGui::GetStyle().ItemSpacing.x)
        / 2.0f;

    if (ImGui::Button("[Reset]", { btn_w, 0 })) {
        g_cfg = Config{};
        g_esp.interp_delay_ms = 35.0f;
        apply_bhop_config();
    }
    ImGui::SameLine();
    if (ImGui::Button("[Exit]", { btn_w, 0 })) g_running = false;

    ImGui::TextDisabled("INSERT - menu    F9 - exit");

    ImGui::End();
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    OrbitalLog("=== orbital start ===");

    Overlay::enable_dpi_awareness();
    OrbitalLog("dpi awareness set");

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

        if (GetAsyncKeyState(VK_INSERT) & 1) g_menu_open = !g_menu_open;
        if (GetAsyncKeyState(VK_F9) & 1)     g_running   = false;

        refresh_game_window();
        overlay.sync_to_game();

        overlay.begin_frame();
        ImDrawList* dl = ImGui::GetBackgroundDrawList();

        {
            const bool game_ok = (g_cs2_hwnd && IsWindow(g_cs2_hwnd));
            char line1[192], line2[192];
            std::snprintf(line1, sizeof(line1),
                "Orbital | viewport %dx%d | game %s",
                g_screen_w, g_screen_h, game_ok ? "ok" : "MISSING");
            std::snprintf(line2, sizeof(line2),
                "%d slots  %d players  |  INSERT menu  F9 exit",
                g_esp.diag_slots, g_esp.players_alive);

            const ImU32 c1 = game_ok ? IM_COL32(120, 255, 120, 255)
                                     : IM_COL32(255, 170, 0, 255);
            const float y0 = static_cast<float>(g_screen_h) - 30.0f;
            dl->AddText({ 8.0f, y0 },        c1, line1);
            dl->AddText({ 8.0f, y0 + 14.0f }, c1, line2);
        }

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
    g_running = false;
    mem_t.join();
    Bhop_Shutdown();
    overlay.cleanup();
    g_mem.detach();

    if (g_log) { fclose(g_log); g_log = nullptr; }
    return 0;
}
