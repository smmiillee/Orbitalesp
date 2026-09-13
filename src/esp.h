// --- src/esp.h ---
#pragma once
#include <vector>
#include <mutex>
#include "memory.h"
#include "offsets.h"

struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };
struct Matrix4x4 { float m[4][4]; };

struct PlayerESP {
    Vec3  origin;
    Vec2  screen_head;
    Vec2  screen_feet;
    int   health;
    int   team;
    bool  visible;
    float distance;
    float box_h;
    float box_w;
};

class ESP {
public:
    std::vector<PlayerESP> players;
    std::mutex             players_mutex;

    void update(const Memory& mem, uintptr_t client_base, int screen_w, int screen_h);

    static bool world_to_screen(const Vec3& world, Vec2& screen,
                                 const Matrix4x4& vm, int screen_w, int screen_h);
};
