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
#include "radar.h"

// ══ NON-STATIC BY REQUIREMENT ════════════════════════════════════════════
// aim.cpp declares g_esp, g_local_team, g_screen_w and g_screen_h as extern so
// the trigger reuses the same ESP instance, the same local-team value and the
// same viewport. A `static` at file scope gives INTERNAL linkage, so those
// symbols would not exist for the linker at all -- that was the LNK2019 pair.
ESP g_esp;
int g_screen_w = 1920;
int g_screen_h = 1080;
int g_local_team = 0;

// These are extern'd by overlay.cpp.
bool g_menu_open = false;
bool g_vsync = false;
HWND g_cs2_hwnd = nullptr;

static bool g_running = true;
static int  g_detected_hz = 0;
static bool g_limit_fps = true;
static int  g_fps_override = 0;

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

static double now_ms() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0).count();
}

// ── clipboard ────────────────────────────────────────────────────────────
static void copy_to_clipboard(const char* text) {
    if (!OpenClipboard(nullptr)) return;
    EmptyClipboard();
    const size_t n = std::strlen(text) + 1;
    if (HGLOBAL mem = GlobalAlloc(GMEM_MOVEABLE, n)) {
        if (void* dst = GlobalLock(mem)) {
            std::memcpy(dst, text, n);
            GlobalUnlock(mem);
            SetClipboardData(CF_TEXT, mem);
        }
    }
    CloseClipboard();
}

struct Config {
    // ESP
    bool  esp_boxes = true, esp_skeleton = true, esp_head_dot = true;
    bool  esp_name = true, esp_weapon = true, esp_health = true;
    bool  esp_distance = true, esp_bomb = true;
    bool  esp_show_enemies = true, esp_show_team = false;
    float box_thickness = 0.5f;
    bool  team_colors = false;

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

    // Menu skin -- bg and checkmark are the two new ones.
    ImVec4 menu_title  = { 0.00f, 0.00f, 0.70f, 1.00f };
    ImVec4 menu_bg     = { 0.78f, 0.78f, 0.78f, 0.97f };
    ImVec4 menu_button = { 0.88f, 0.80f, 0.55f, 1.00f };
    ImVec4 menu_slider = { 0.55f, 0.55f, 0.55f, 1.00f };
    ImVec4 menu_check  = { 0.00f, 0.00f, 0.00f, 1.00f };

    // Features
    bool  bh_enabled = false;
    bool  aim_enabled = false, aim_fire = true;
    int   aim_key = 0;
    float aim_radius = 1.4f;
    bool  jb_enabled = false;
    int   jb_key = 0;                 // 0 = unbound = off
    float jb_crouch_lead = 31.0f;     // two ticks
    float jb_uncrouch_height = 11.0f; // documented 9-11 unit window

    int   aim_delay = 0;
    int   radar_port = 3000;
    int   radar_sel = 0;
} g_cfg;

// ── config file ──────────────────────────────────────────────────────────
static void save_config() {
    wchar_t path[MAX_PATH]{};
    sidecar_path(path, MAX_PATH, L"orbital.cfg");
    FILE* f = nullptr;
    _wfopen_s(&f, path, L"w");
    if (!f) { OrbitalLog("config save FAILED"); return; }

// The macro parameter is 'field', NOT 'f'. Using 'f' collides with the FILE*
// argument, so fprintf(f, ...) expands to fprintf(esp_boxes, ...) and the field
// name gets reported as an undeclared identifier.
#define W_B(field) fprintf(f, #field "=%d\n", g_cfg.field ? 1 : 0)
#define W_I(field) fprintf(f, #field "=%d\n", g_cfg.field)
#define W_F(field) fprintf(f, #field "=%.4f\n", g_cfg.field)
#define W_C(field) fprintf(f, #field "=%.4f %.4f %.4f %.4f\n", \
                           g_cfg.field.x, g_cfg.field.y, \
                           g_cfg.field.z, g_cfg.field.w)

    W_B(esp_boxes); W_B(esp_skeleton); W_B(esp_head_dot);
    W_B(esp_name); W_B(esp_weapon); W_B(esp_health);
    W_B(esp_distance); W_B(esp_bomb);
    W_B(esp_show_enemies); W_B(esp_show_team);
    W_F(box_thickness); W_B(team_colors);

    W_C(color_enemy); W_C(color_team); W_C(color_box);
    W_C(color_skel); W_C(color_head); W_C(color_name);
    W_C(color_weapon); W_C(color_dist); W_C(color_bomb);
    W_C(color_carrier);

    W_C(menu_title); W_C(menu_bg); W_C(menu_button);
    W_C(menu_slider); W_C(menu_check);

    W_B(bh_enabled);
    W_B(aim_enabled); W_B(aim_fire); W_I(aim_key); W_F(aim_radius);
    W_B(jb_enabled); W_I(jb_key);
    W_F(jb_crouch_lead); W_F(jb_uncrouch_height);
    W_I(aim_delay); W_I(radar_port); W_I(radar_sel);

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

        auto asB = [&](bool& v)  { v = (atoi(val) != 0); };
        auto asI = [&](int& v)   { v = atoi(val); };
        auto asF = [&](float& v) { v = (float)atof(val); };
        auto asC = [&](ImVec4& v) {
            float r = 0, g = 0, b = 0, a = 1;
            sscanf_s(val, "%f %f %f %f", &r, &g, &b, &a);
            v = ImVec4(r, g, b, a);
        };

        if      (!strcmp(key,"esp_boxes"))        asB(g_cfg.esp_boxes);
        else if (!strcmp(key,"esp_skeleton"))     asB(g_cfg.esp_skeleton);
        else if (!strcmp(key,"esp_head_dot"))     asB(g_cfg.esp_head_dot);
        else if (!strcmp(key,"esp_name"))         asB(g_cfg.esp_name);
        else if (!strcmp(key,"esp_weapon"))       asB(g_cfg.esp_weapon);
        else if (!strcmp(key,"esp_health"))       asB(g_cfg.esp_health);
        else if (!strcmp(key,"esp_distance"))     asB(g_cfg.esp_distance);
        else if (!strcmp(key,"esp_bomb"))         asB(g_cfg.esp_bomb);
        else if (!strcmp(key,"esp_show_enemies")) asB(g_cfg.esp_show_enemies);
        else if (!strcmp(key,"esp_show_team"))    asB(g_cfg.esp_show_team);
        else if (!strcmp(key,"box_thickness"))    asF(g_cfg.box_thickness);
        else if (!strcmp(key,"team_colors"))      asB(g_cfg.team_colors);
        else if (!strcmp(key,"color_enemy"))      asC(g_cfg.color_enemy);
        else if (!strcmp(key,"color_team"))       asC(g_cfg.color_team);
        else if (!strcmp(key,"color_box"))        asC(g_cfg.color_box);
        else if (!strcmp(key,"color_skel"))       asC(g_cfg.color_skel);
        else if (!strcmp(key,"color_head"))       asC(g_cfg.color_head);
        else if (!strcmp(key,"color_name"))       asC(g_cfg.color_name);
        else if (!strcmp(key,"color_weapon"))     asC(g_cfg.color_weapon);
        else if (!strcmp(key,"color_dist"))       asC(g_cfg.color_dist);
        else if (!strcmp(key,"color_bomb"))       asC(g_cfg.color_bomb);
        else if (!strcmp(key,"color_carrier"))    asC(g_cfg.color_carrier);
        else if (!strcmp(key,"menu_title"))       asC(g_cfg.menu_title);
        else if (!strcmp(key,"menu_bg"))          asC(g_cfg.menu_bg);
        else if (!strcmp(key,"menu_button"))      asC(g_cfg.menu_button);
        else if (!strcmp(key,"menu_slider"))      asC(g_cfg.menu_slider);
        else if (!strcmp(key,"menu_check"))       asC(g_cfg.menu_check);
        else if (!strcmp(key,"bh_enabled"))       asB(g_cfg.bh_enabled);
        else if (!strcmp(key,"aim_enabled"))      asB(g_cfg.aim_enabled);
        else if (!strcmp(key,"aim_fire"))         asB(g_cfg.aim_fire);
        else if (!strcmp(key,"aim_key"))          asI(g_cfg.aim_key);
        else if (!strcmp(key,"aim_radius"))       asF(g_cfg.aim_radius);
        else if (!strcmp(key,"jb_enabled"))       asB(g_cfg.jb_enabled);
        else if (!strcmp(key,"jb_key"))           asI(g_cfg.jb_key);
        else if (!strcmp(key,"jb_crouch_lead"))   asF(g_cfg.jb_crouch_lead);
        else if (!strcmp(key,"jb_uncrouch_height")) asF(g_cfg.jb_uncrouch_height);
        else if (!strcmp(key,"aim_delay"))        asI(g_cfg.aim_delay);
        else if (!strcmp(key,"radar_port"))       asI(g_cfg.radar_port);
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
    const int hz = (int)dm.dmDisplayFrequency;
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
// match landed us on a 192x456 console window, so we enumerate and pick best.
struct WindowCand { HWND hwnd; int w, h; bool titled; };

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
    const int w = r.right - r.left, hh = r.bottom - r.top;
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
        long long s = (long long)c.w * c.h;
        if (!c.titled) s /= 4;
        if (c.w < 640 || c.h < 480) s /= 4;
        if (s > best_score) { best_score = s; best = c.hwnd; }
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
    OrbitalLog("game window chosen 0x%p %dx%d", h, g_screen_w, g_screen_h);
    return true;
}

static void use_monitor_size() {
    const int w = GetSystemMetrics(SM_CXSCREEN), h = GetSystemMetrics(SM_CYSCREEN);
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
    Aim_SetRadius(g_cfg.aim_radius);
    Aim_SetDelay(g_cfg.aim_delay);
    Movement_SetJumpbug(g_cfg.jb_enabled);
    Movement_SetKey(g_cfg.jb_key);
    Movement_SetCrouchLead(g_cfg.jb_crouch_lead);
    Movement_SetUncrouchHeight(g_cfg.jb_uncrouch_height);
    Radar_SetPort(g_cfg.radar_port);
    Radar_SetSelection(g_cfg.radar_sel);
}

void memory_thread() {
    while (g_running && !g_mem.attach(L"cs2.exe"))
        wait_until(std::chrono::steady_clock::now() + std::chrono::seconds(2));

    OrbitalLog("memory attach: %s", g_mem.is_valid() ? "ok" : "FAILED");
    Bhop_Init();
    Aim_Init();
    Movement_Init();
    Radar_Init();
    apply_feature_config();

    while (g_running) {
        if (g_mem.is_valid()) {
            const uintptr_t lp = g_mem.read<uintptr_t>(
                g_mem.client_dll + offsets::dwLocalPlayerPawn);
            if (lp) g_local_team = g_mem.read<uint8_t>(lp + offsets::m_iTeamNum);
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
        const bool is_team = (g_local_team != 0 && p.team == g_local_team);
        if (!is_team && !g_cfg.esp_show_enemies) continue;
        if (is_team  && !g_cfg.esp_show_team)    continue;

        const ImVec4 tcol = is_team ? g_cfg.color_team : g_cfg.color_enemy;
        auto pick = [&](const ImVec4& own) -> ImU32 {
            return col_of(g_cfg.team_colors ? tcol : own);
        };

        const float cx = p.screen_head.x, top = p.screen_top.y;
        const float bot = p.screen_feet.y, bh = p.box_h, bw = p.box_w;

        if (cx + bw / 2.0f < 0 || cx - bw / 2.0f > g_screen_w) continue;
        if (bot < 0 || top > g_screen_h) continue;

        float gap = bh * 0.06f;
        if (gap < 2.0f) gap = 2.0f;
        if (gap > 8.0f) gap = 8.0f;

        if (g_cfg.esp_boxes)
            dl->AddRect({ cx - bw / 2.0f, top }, { cx + bw / 2.0f, bot },
                        pick(g_cfg.color_box), 0.0f, 0, g_cfg.box_thickness);

        if (g_cfg.esp_skeleton && p.has_bones) {
            const ImU32 sk = pick(g_cfg.color_skel);
            for (const auto& l : kSkeleton) {
                if (!p.bone_ok[l[0]] || !p.bone_ok[l[1]]) continue;
                dl->AddLine({ p.bones[l[0]].x, p.bones[l[0]].y },
                            { p.bones[l[1]].x, p.bones[l[1]].y },
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
            const float bh2 = bh * (p.health / 100.0f);
            const float bx = cx - bw / 2.0f - 6.0f;
            const ImU32 hp = IM_COL32((int)(255 * (1.0f - p.health / 100.0f)),
                                      (int)(255 * (p.health / 100.0f)), 0, 255);
            dl->AddRectFilled({ bx, bot - bh2 }, { bx + 3.0f, bot }, hp);
            dl->AddRect({ bx, top }, { bx + 3.0f, bot }, IM_COL32(0,0,0,180));
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
            const float y = bot + gap +
                ((g_cfg.esp_weapon && p.weapon[0]) ? lh + gap : 0.0f);
            dl->AddText({ cx - sz.x * 0.5f, y }, col_of(g_cfg.color_carrier), tag);
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
                        { b.screen.x + h / 2.0f, b.screen.y },
                        c, 0.0f, 0, g_cfg.box_thickness);
            dl->AddText({ b.screen.x - 7.0f, b.screen_top.y - 16.0f }, c, "C4");
            char buf[24];
            std::snprintf(buf, sizeof(buf), "%.0fm", b.distance);
            dl->AddText({ b.screen.x - 14.0f, b.screen.y + 2.0f }, c, buf);
        }
    }
}

// ── keybind capture ──────────────────────────────────────────────────────
enum { CAPTURE_NONE = -1, CAPTURE_AIM = 0, CAPTURE_JUMPBUG = 1 };

static int  g_capture = CAPTURE_NONE;
static bool g_capture_wait_release = false;

static const char* vk_name(int vk) {
    static char buf[32];
    switch (vk) {
        case 0:            return "none (always)";
        case VK_LBUTTON:   return "MOUSE1";
        case VK_RBUTTON:   return "MOUSE2";
        case VK_MBUTTON:   return "MOUSE3";
        case VK_XBUTTON1:  return "MOUSE4";
        case VK_XBUTTON2:  return "MOUSE5";
        case VK_MENU:      return "ALT";
        case VK_SHIFT:     return "SHIFT";
        case VK_CONTROL:   return "CTRL";
        case VK_CAPITAL:   return "CAPS";
        case VK_SPACE:     return "SPACE";
        case VK_TAB:       return "TAB";
        default: break;
    }
    const UINT sc = MapVirtualKeyW((UINT)vk, MAPVK_VK_TO_VSC);
    if (sc && GetKeyNameTextA((LONG)(sc << 16), buf, sizeof(buf)) > 0)
        return buf;
    std::snprintf(buf, sizeof(buf), "0x%02X", vk);
    return buf;
}

// Returns the newly pressed key, or 0. Everything must be released first so the
// click that opened capture isn't recorded as the bind.
static int capture_poll() {
    if (g_capture == CAPTURE_NONE) return 0;

    if (g_capture_wait_release) {
        for (int vk = 1; vk < 255; ++vk)
            if (GetAsyncKeyState(vk) & 0x8000) return 0;
        g_capture_wait_release = false;
        return 0;
    }

    static const int kMouse[] = { VK_LBUTTON, VK_RBUTTON, VK_MBUTTON,
                                  VK_XBUTTON1, VK_XBUTTON2 };
    for (int vk : kMouse)
        if (GetAsyncKeyState(vk) & 0x8000) return vk;

    for (int vk = 0x08; vk < 255; ++vk)
        if (GetAsyncKeyState(vk) & 0x8000) return vk;

    return 0;
}

// label + current bind + a "set" button that captures the next input.
//
// UNBOUND MEANS OFF. There is no "always on": an unbound feature does nothing
// until you bind it, which is shown in orange so it cannot be mistaken for
// working.
static void keybind_row(const char* label, int* vk, int target) {
    const bool unbound = (*vk == 0);

    ImGui::TextDisabled("%s", label);
    ImGui::SameLine(150.0f);

    if (g_capture == target)
        ImGui::TextColored({ 1.0f, 0.6f, 0.1f, 1.0f }, "press any input...");
    else if (unbound)
        ImGui::TextColored({ 1.0f, 0.5f, 0.2f, 1.0f }, "not bound");
    else
        ImGui::Text("%s", vk_name(*vk));

    ImGui::SameLine(280.0f);
    char id[32];
    std::snprintf(id, sizeof(id), "set##%d", target);
    if (ImGui::SmallButton(id)) {
        g_capture = target;
        g_capture_wait_release = true;
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Click, then press any key or mouse button.\n"
                          "HOLD that key to enable the feature.\n"
                          "ESC leaves it unbound, which means OFF.");

    if (g_capture == target) {
        ImGui::SameLine();
        if (ImGui::SmallButton("x")) {
            g_capture = CAPTURE_NONE;
            *vk = 0;
        }
    }

    if (unbound && g_capture != target) {
        ImGui::SameLine();
        ImGui::TextDisabled("(off until bound)");
    }
}

static void apply_captured(int vk) {
    if (g_capture == CAPTURE_AIM) {
        g_cfg.aim_key = vk;
        Aim_SetKey(vk);
    } else if (g_capture == CAPTURE_JUMPBUG) {
        g_cfg.jb_key = vk;
        Movement_SetKey(vk);
    }
    g_capture = CAPTURE_NONE;
}

// ── colour rows: right-click copies, middle-click pastes ─────────────────
static char   g_copy_flash[64] = "";
static double g_copy_flash_at = -1e9;

// Accepts "#RRGGBB", "#RRGGBBAA", "R,G,B" or "R,G,B,A".
static bool paste_color_from_clipboard(ImVec4* c) {
    if (!OpenClipboard(nullptr)) return false;

    bool ok = false;
    if (HANDLE h = GetClipboardData(CF_TEXT)) {
        if (const char* txt = (const char*)GlobalLock(h)) {
            unsigned r = 0, g = 0, b = 0, a = 255;
            int got = 0;

            // Skip leading whitespace.
            while (*txt == ' ' || *txt == '\t' || *txt == '\r' || *txt == '\n')
                ++txt;

            // sscanf_s is a Microsoft extension, NOT a std:: member -- so it
            // must be unqualified here.
            if (*txt == '#') {
                sscanf_s(txt + 1, "%2x%2x%2x%2x", &r, &g, &b, &a);
                got = (int)std::strlen(txt + 1) >= 8 ? 4 : 3;
            } else {
                got = sscanf_s(txt, "%u,%u,%u,%u", &r, &g, &b, &a);
            }
            if (got < 3) { r = g = b = 0; a = 255; got = 0; }

            if (got >= 3) {
                if (got < 4) a = 255;
                if (r > 255) r = 255;
                if (g > 255) g = 255;
                if (b > 255) b = 255;
                if (a > 255) a = 255;
                c->x = r / 255.0f;
                c->y = g / 255.0f;
                c->z = b / 255.0f;
                c->w = a / 255.0f;
                ok = true;
            }
            GlobalUnlock(h);
        }
    }
    CloseClipboard();
    return ok;
}

static void color_row(const char* id, const char* label, ImVec4* c) {
    ImGui::ColorEdit4(id, &c->x,
        ImGuiColorEditFlags_NoInputs | ImGuiColorEditFlags_NoLabel |
        ImGuiColorEditFlags_AlphaPreviewHalf);
    const bool hovered = ImGui::IsItemHovered();

    char hex[16];
    std::snprintf(hex, sizeof(hex), "#%02X%02X%02X%02X",
        (int)(c->x * 255.0f + 0.5f), (int)(c->y * 255.0f + 0.5f),
        (int)(c->z * 255.0f + 0.5f), (int)(c->w * 255.0f + 0.5f));

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        copy_to_clipboard(hex);
        std::snprintf(g_copy_flash, sizeof(g_copy_flash), "%s  %s copied",
                      label, hex);
        g_copy_flash_at = now_ms();
    }

    if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Middle)) {
        if (paste_color_from_clipboard(c))
            std::snprintf(g_copy_flash, sizeof(g_copy_flash),
                          "%s  pasted", label);
        else
            std::snprintf(g_copy_flash, sizeof(g_copy_flash),
                          "%s  clipboard had no colour", label);
        g_copy_flash_at = now_ms();
    }

    if (hovered)
        ImGui::SetTooltip("right-click: copy %s\nmiddle-click: paste colour",
                          hex);

    ImGui::SameLine();
    ImGui::Text("%s", label);
}

// ── tabs ─────────────────────────────────────────────────────────────────

static void tab_esp() {
    const float col_w = ImGui::GetContentRegionAvail().x * 0.5f;
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

    // Status for the two features still being brought up. Bones drive the
    // skeleton AND the triggerbot, so if this says scanning the trigger cannot
    // work regardless of its own code.
    ImGui::Separator();
    if (g_esp.diag_bones_ok)
        ImGui::TextDisabled("bones: ok 0x%llX/0x%llX",
            (unsigned long long)g_esp.diag_bone_node,
            (unsigned long long)g_esp.diag_bone_arr);
    else
        ImGui::TextColored({ 1.0f, 0.6f, 0.1f, 1.0f }, "bones: scanning...");

    if (g_esp.diag_wsvc_ok)
        ImGui::TextDisabled("weapon svc: 0x%llX (verified)",
            (unsigned long long)g_esp.diag_wsvc);
    else
        ImGui::TextColored({ 1.0f, 0.6f, 0.1f, 1.0f },
                           "weapon svc: not found");

    if (g_esp.diag_defidx)
        ImGui::TextDisabled("def index off: 0x%X (calibrated)",
                            g_esp.diag_defidx);
    else
        ImGui::TextColored({ 1.0f, 0.6f, 0.1f, 1.0f },
                           "def index: calibrating...");

    if (g_esp.diag_c4_ent)
        ImGui::TextDisabled("C4 ent: 0x%llX",
            (unsigned long long)g_esp.diag_c4_ent);
    else
        ImGui::TextDisabled("C4 ent: none planted");
}

static void tab_aim() {
    ImGui::TextDisabled("[ Triggerbot ]");
    ImGui::TextDisabled("detection: read-only     firing: injects a click");
    ImGui::Separator();

    if (ImGui::Checkbox("Triggerbot", &g_cfg.aim_enabled))
        Aim_SetEnabled(g_cfg.aim_enabled);

    if (ImGui::Checkbox("Firing (injects left click)", &g_cfg.aim_fire))
        Aim_SetFire(g_cfg.aim_fire);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Unticked: still detects and shows the indicator,\n"
                          "but never clicks. That makes it read-only.");

    ImGui::SetNextItemWidth(240.0f);
    if (ImGui::SliderFloat("##arad", &g_cfg.aim_radius, 0.2f, 6.0f,
                           "radius %.2f%% of height"))
        Aim_SetRadius(g_cfg.aim_radius);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How close to the crosshair a bone must be.\n"
                          "~1.4%% of height is head-sized.");

    ImGui::SetNextItemWidth(240.0f);
    if (ImGui::SliderInt("##adelay", &g_cfg.aim_delay, 0, 600,
                         "delay %d ms"))
        Aim_SetDelay(g_cfg.aim_delay);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Target must stay acquired this long before firing,\n"
                          "measured from re-acquiring. 0 = instant.\n"
                          "The cooldown is separate and starts after firing.");

    ImGui::Spacing();
    keybind_row("arm key", &g_cfg.aim_key, CAPTURE_AIM);

    const AimDebug ad = Aim_GetDebug();
    ImGui::Separator();
    if (ad.on_target)
        ImGui::TextColored({ 0.1f, 0.6f, 0.1f, 1.0f },
                           "on target: %s  (%.0f px)", ad.target_bone, ad.target_dist);
    else
        ImGui::TextDisabled("on target: no");
    ImGui::TextDisabled("firing: %s", ad.firing ? "yes" : "no");
}

static void tab_movement() {
    ImGui::TextDisabled("[ Jumpbug ]");
    ImGui::TextDisabled("injects CTRL; nothing is written to CS2");
    ImGui::Separator();

    if (ImGui::Checkbox("Jumpbug", &g_cfg.jb_enabled))
        Movement_SetJumpbug(g_cfg.jb_enabled);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("Crouches just before landing, holds through\n"
                          "touchdown, releases after. Cancels fall damage\n"
                          "reliably; gains height on frames that line up.");

    ImGui::Spacing();
    keybind_row("arm key", &g_cfg.jb_key, CAPTURE_JUMPBUG);

    ImGui::SetNextItemWidth(240.0f);
    if (ImGui::SliderFloat("##jbc", &g_cfg.jb_crouch_lead, 4.0f, 200.0f,
                           "crouch %.0f ms before landing"))
        Movement_SetCrouchLead(g_cfg.jb_crouch_lead);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("How early to crouch. 31 ms is two ticks, which is\n"
                          "what the reference jumpbug uses.");

    ImGui::SetNextItemWidth(240.0f);
    if (ImGui::SliderFloat("##jbh", &g_cfg.jb_uncrouch_height, 2.0f, 30.0f,
                           "uncrouch at %.0f units above ground"))
        Movement_SetUncrouchHeight(g_cfg.jb_uncrouch_height);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("This is the real timing control, and it is a HEIGHT\n"
                          "rather than a delay, because the ms window depends on\n"
                          "fall speed. The documented jumpbug window is 9-11\n"
                          "units above the ground; 11 triggers on entry.");

    const MovementDebug md = Movement_GetDebug();
    ImGui::Separator();
    ImGui::Text("ground: %s   crouching: %s   jumping: %s",
        md.on_ground ? "YES" : "no", md.crouching ? "yes" : "no",
        md.jumping ? "yes" : "no");

    // game_ducked is the GAME's own crouch flag. If we are crouching but the
    // game does not think we are, our CTRL never registered and no timing
    // change will help.
    if (md.crouching && !md.game_ducked)
        ImGui::TextColored({ 1.0f, 0.4f, 0.0f, 1.0f },
                           "game does NOT see the crouch (CTRL not registering)");
    else
        ImGui::TextDisabled("game crouch: %s", md.game_ducked ? "yes" : "no");

    // height is the number that decides the jumpbug, so it is shown always.
    ImGui::TextDisabled("height %.1f units   tti %.1f ms   jumpbugs %d",
        md.height, md.tti, md.jumpbugs);

    ImGui::Separator();
    ImGui::TextDisabled("[ Bhop ]");
    ImGui::TextDisabled("HOLD SPACE - no auto-jump, no keybind");
    if (ImGui::Checkbox("Bhop (space)", &g_cfg.bh_enabled))
        Bhop_SetEnabled(g_cfg.bh_enabled);

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
    const RadarInfo ri = Radar_Get();

    ImGui::TextDisabled("[ Radar ]");
    ImGui::TextDisabled("opens the dashboard in your browser");
    ImGui::Separator();

    // The radar is its OWN program: cs2_dashboard.exe, built from the
    // orbitalweb repo (C++ / CMake, not Node). It must already be running.
    ImGui::TextWrapped("Requires cs2_dashboard.exe from orbitalweb to be "
                       "running. It serves the dashboard on port %d.", ri.port);

    ImGui::Spacing();
    ImGui::TextDisabled("[ address ]");

    // Pick-list instead of a guess. Entry 0 is always 127.0.0.1, which is
    // correct when you are viewing the radar on the same PC as the server.
    static const char* items[RadarInfo::kMaxAddr] = {};
    for (int i = 0; i < ri.count && i < RadarInfo::kMaxAddr; ++i)
        items[i] = ri.label[i];

    ImGui::SetNextItemWidth(320.0f);
    int sel = ri.selected;
    if (ImGui::Combo("##raddr", &sel, items, ri.count)) {
        g_cfg.radar_sel = sel;
        Radar_SetSelection(sel);
    }
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("127.0.0.1 works when the browser is on THIS PC.\n"
                          "Pick a LAN address for a phone or second PC.\n"
                          "The dashboard shows the right IP in its console.");

    ImGui::SetNextItemWidth(160.0f);
    if (ImGui::SliderInt("##rport", &g_cfg.radar_port, 1024, 65535,
                         "port %d"))
        Radar_SetPort(g_cfg.radar_port);

    ImGui::Text("url: %s", ri.url);

    ImGui::Spacing();
    if (ImGui::Button("[Start Radar]", { 200.0f, 0.0f }))
        Radar_Start();

    if (ri.opened)
        ImGui::TextColored({ 0.1f, 0.6f, 0.1f, 1.0f }, "opened in browser");
    if (ri.failed)
        ImGui::TextColored({ 1.0f, 0.4f, 0.0f, 1.0f }, "could not open browser");

    ImGui::Separator();
    ImGui::TextDisabled("If the page does not load:");
    ImGui::TextDisabled(" - is cs2_dashboard.exe running?");
    ImGui::TextDisabled(" - on another PC, is port %d allowed through", ri.port);
    ImGui::TextDisabled("   Windows Firewall on the server PC?");
}

static void tab_colors() {
    ImGui::Checkbox("Use team colours", &g_cfg.team_colors);
    ImGui::TextDisabled("right-click any swatch to copy the hex");
    ImGui::Separator();

    const float col_w = ImGui::GetContentRegionAvail().x * 0.5f;
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
    color_row("##mm", "Background",   &g_cfg.menu_bg);
    color_row("##mb", "Buttons",      &g_cfg.menu_button);

    ImGui::NextColumn();
    color_row("##ms", "Slider bg",    &g_cfg.menu_slider);
    color_row("##mk", "Check marks",  &g_cfg.menu_check);

    ImGui::Columns(1);
}

static void tab_misc() {
    ImGui::TextDisabled("[ Config ]");
    ImGui::TextDisabled("orbital.cfg next to the exe");
    ImGui::TextDisabled("also saved automatically on exit");
    ImGui::Spacing();
    ImGui::Spacing();

    const float w = ImGui::GetContentRegionAvail().x;
    const float bw = w / 3.0f;

    if (ImGui::Button("[Save]", { bw, 0 })) save_config();
    ImGui::SameLine();
    if (ImGui::Button("[Reset]", { bw, 0 })) {
        g_cfg = Config{};
        g_esp.interp_delay_ms = 35.0f;
        apply_feature_config();
    }
    ImGui::SameLine();
    if (ImGui::Button("[Exit]", { bw, 0 })) g_running = false;
}

void render_menu() {
    // Menu skin, reapplied each frame so the pickers are live.
    {
        ImGuiStyle& st = ImGui::GetStyle();
        st.Colors[ImGuiCol_TitleBg]       = g_cfg.menu_title;
        st.Colors[ImGuiCol_TitleBgActive] = g_cfg.menu_title;
        st.Colors[ImGuiCol_WindowBg]      = g_cfg.menu_bg;
        st.Colors[ImGuiCol_Button]        = g_cfg.menu_button;
        st.Colors[ImGuiCol_SliderGrab]    = g_cfg.menu_slider;
        st.Colors[ImGuiCol_CheckMark]     = g_cfg.menu_check;
    }

    // Keybind capture runs once per frame, before any widget is built, so the
    // captured key cannot also trigger a button in the same frame.
    {
        const int vk = capture_poll();
        if (vk) {
            if (vk == VK_ESCAPE) {
                int* target = nullptr;
                if (g_capture == CAPTURE_AIM)     target = &g_cfg.aim_key;
                if (g_capture == CAPTURE_JUMPBUG) target = &g_cfg.jb_key;
                if (target) *target = 0;
                apply_captured(0);
            } else {
                apply_captured(vk);
            }
        }
    }

    ImGui::SetNextWindowSize({ 620.0f, 560.0f }, ImGuiCond_Once);
    ImGui::SetNextWindowSizeConstraints({ 520.0f, 320.0f }, { 1000.0f, 900.0f });
    ImGui::SetNextWindowPos({ 20.0f, 20.0f }, ImGuiCond_Once);
    ImGui::Begin("Orbital - Mars", nullptr);

    if (!g_mem.is_valid())
        ImGui::TextColored({ 1.0f, 0.5f, 0.0f, 1.0f }, "[ waiting for CS2 ]");
    else
        ImGui::TextColored({ 0.0f, 0.7f, 0.0f, 1.0f },
            "[ attached  %dx%d  %d Hz  %d players ]",
            g_screen_w, g_screen_h, g_detected_hz, g_esp.players_alive);

    ImGui::Separator();

    if (ImGui::BeginTabBar("##orbital_tabs", ImGuiTabBarFlags_None)) {
        if (ImGui::BeginTabItem("ESP"))      { tab_esp();      ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("AIM"))      { tab_aim();      ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("MOVEMENT")) { tab_movement(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("COLORS"))   { tab_colors();   ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("MISC"))     { tab_misc();     ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }

    if (now_ms() - g_copy_flash_at < 1500.0)
        ImGui::TextColored({ 0.1f, 0.6f, 0.1f, 1.0f }, "%s", g_copy_flash);

    ImGui::End();
}

int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) {
    OrbitalLog("=== orbital start ===");
    Overlay::enable_dpi_awareness();
    load_config();

    bool have = false;
    for (int i = 0; i < 20 && !have; ++i) {
        have = refresh_game_window();
        if (!have) std::this_thread::sleep_for(std::chrono::milliseconds(250));
    }
    if (!have) {
        OrbitalLog("game window NOT found after 5s - using monitor size");
        use_monitor_size();
    }
    g_detected_hz = query_refresh_rate(g_cs2_hwnd);
    OrbitalLog("refresh %d Hz", g_detected_hz);

    Overlay overlay;
    if (!overlay.create(g_screen_w, g_screen_h)) {
        OrbitalLog("overlay.create FAILED");
        return 1;
    }
    OrbitalLog("overlay created %dx%d", g_screen_w, g_screen_h);

    std::thread mem_t(memory_thread);
    OrbitalLog("entering message loop");

    MSG msg{};
    while (g_running) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
            if (msg.message == WM_QUIT) g_running = false;
        }

        const bool cs2_active =
            (g_cs2_hwnd && IsWindow(g_cs2_hwnd) &&
             GetForegroundWindow() == g_cs2_hwnd);

        // Losing focus closes the menu, so it can never sit over another app.
        if (!cs2_active && g_menu_open) {
            g_menu_open = false;
            g_capture = CAPTURE_NONE;
        }

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
                const auto n = std::chrono::steady_clock::now();
                if (next < n) next = n;
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
    Radar_Shutdown();
    overlay.cleanup();
    g_mem.detach();
    if (g_log) { fclose(g_log); g_log = nullptr; }
    return 0;
}
