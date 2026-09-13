// --- src/esp.h ---
#pragma once
#include <vector>
#include <mutex>
#include "memory.h"
#include "offsets.h"

struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };
struct Matrix4x4 { float m[4][4]; };

// Only world-space data stored — screen projection done fresh each render frame
struct PlayerData {
    Vec3  origin;   // feet world position
    int   health;
    int   team;
    float distance;
};

// Screen-space result computed fresh every render frame
struct PlayerESP {
    Vec2  screen_head;
    Vec2  screen_feet;
    int   health;
    int   team;
    float distance;
    float box_h;
    float box_w;
};

class ESP {
public:
    // World-space data updated by memory thread
    std::vector<PlayerData> world_players;
    std::mutex              world_mutex;

    // Called by memory thread — stores world positions only
    void update_world(const Memory& mem, uintptr_t client_base);

    // Called by render thread — reads fresh view matrix, projects to screen
    std::vector<PlayerESP> project(const Memory& mem, uintptr_t client_base,
                                    int screen_w, int screen_h);

    static bool world_to_screen(const Vec3& world, Vec2& screen,
                                 const Matrix4x4& vm, int screen_w, int screen_h);
};
