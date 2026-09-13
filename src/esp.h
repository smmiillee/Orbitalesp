#pragma once
#include <vector>
#include <mutex>
#include "memory.h"
#include "offsets.h"

struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };
struct Matrix4x4 { float m[4][4]; };

// Store world data
struct PlayerData {
    Vec3 origin; // feet world position
    Vec3 head;   // head world position
    int health;
    int team;
    float distance;
    bool drawn;
};

// Bone skeleton data
struct Bone {
    int index;          // bone index
    Vec3 screen_pos;    // screen coordinates
    float line_width;
};

// Final ESP render herostruct
struct PlayerESP {
    Vec2 screen_head;
    Vec2 screen_feet;
    Vec2 screen_head_dots;
    int health;
    int team;
    float distance;
    float box_h;
    float box_w;
    std::vector<Bone> bones;   // Skeleton bones
    std::vector<Vec2> corners; // Corner ESP
    std::vector<Vec2> head_markers; // Head dot markers
    bool draw_skeleton;
    bool draw_head_markers;
    bool draw_corners;
};

class ESP {
public:
    // World-space botl stored by memory thread
    std::vector<PlayerData> world_players;
    std::mutex world_mutex;

    // Callel by memory thread — stores world positions ONLY
    void update_world(const Memory& mem, uintptr_t client_base);

    // Called by render thread — reads fresh view matrix, projects to screen
    std::vector<PlayerESP> project(
        const Memory& mem, uintptr_t client_base,
        int screen_w, int screen_h,
        bool draw_skeleton, bool draw_head_markers, bool draw_corners
    );

    static bool world_to_screen(const Vec3& world, Vec2& screen,
        const Matrix4x4& vm, int screen_w, int screen_h);

    // Helper: draw head dot marker (red for enemy, green for teammate)
    static void draw_head_dots(ImDrawList* dl, const std::vector<PlayerESP>& esp,
        int screen_w, int screen_h, const ImVec4& color_enemy, const ImVec4& color_team);

    // Helper: draw skeleton bones
    static void draw_skeleton(ImDrawList* dl, const PlayerESP& p, const ImVec4& color);

    // Helper: draw corner brackets and lines
    static void draw_corners_lines(ImDrawList* dl, const PlayerESP& p, const ImVec4& color);
};
