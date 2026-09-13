// --- src/esp.h ---
#pragma once
#include <vector>
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

    // Debug counters — visible in menu so we know exactly where the chain breaks
    int debug_total_controllers = 0;
    int debug_valid_pawns       = 0;
    int debug_alive             = 0;
    int debug_positioned        = 0;
    int debug_on_screen         = 0;

    void update(const Memory& mem, uintptr_t client_base);

    static bool world_to_screen(const Vec3& world, Vec2& screen,
                                 const Matrix4x4& vm, int screen_w, int screen_h);
private:
    uintptr_t get_entity(const Memory& mem, uintptr_t entity_list, int index) const;
    uintptr_t resolve_handle(const Memory& mem, uintptr_t entity_list, uint32_t handle) const;
};
