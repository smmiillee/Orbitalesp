#pragma once
#include <vector>
#include <mutex>
#include "memory.h"
#include "offsets.h"

struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };
struct Matrix4x4 { float m[4][4]; };

struct PlayerData {
    Vec3 origin;
    Vec3 head;
    int health;
    int team;
    float distance;
};

struct Bone { int index; Vec2 screen_pos; float h; };

struct PlayerESP {
    Vec2 head, feet, head_mtx[3], feet_mtx[2];
    int health; int team; float distance;
    float box_h, box_w;
    std::vector<Bone> bones;
    bool draw_skeleton = true;
    bool draw_head_dots = true;
    bool draw_corners = true;
};

class ESP {
    std::vector<PlayerData> world_plys;
    std::mutex world_mutex;

    void update_world(const Memory& mem, uintptr_t client_base);
    std::vector<PlayerESP> project(const Memory& mem, uintptr_t client_base,
        int screen_w, int screen_h, bool ssk, bool hdr, bool cnr);

    static bool world_to_screen(const Vec3& w, Vec2& s, const Matrix4x4& vm, int sw, int sh);
    static void draw_dots(ImDrawList* dl, const PlayerESP& p, int sw, int sh, const ImVec4& c);
    static void draw_skeleton(ImDrawList* dl, const PlayerESP& p, int sw, int sh, const ImVec4& c);
    static void draw_corners(ImDrawList* dl, const PlayerESP& p, int sw, int sh, const ImVec4& c);
};
