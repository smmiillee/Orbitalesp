// --- src/esp.h ---
#pragma once
#include <cstdint>
#include <mutex>
#include <vector>
#include <imgui.h>

#include "cs2math.h"
#include "sdk.h"

// Written by the memory thread: world-space snapshot of one player.
struct PlayerData {
    Vec3  origin;                  // feet position
    float distance = 0.0f;         // metres from local pawn
    int   health   = 0;
    int   team     = 0;
    bool  bones_ok = false;        // bone data read + sanity-checked
    Vec3  bone_pos[bones::count];  // world space, indexed by bone id
};

// Produced on the render thread: projected, ready to draw.
struct PlayerESP {
    Vec2  head;                    // screen pos of head (real bone 6, or origin+70 fallback)
    Vec2  feet;                    // screen pos of origin
    float box_h = 0.0f;
    float box_w = 0.0f;
    int   health = 0;
    int   team   = 0;
    float distance = 0.0f;
    bool  bones_ok = false;
    Vec2  bone_screen[bones::count];
    bool  bone_ok[bones::count] = {};
};

class ESP {
public:
    // Memory thread: reads controllers -> pawns -> origin + all bones (world space).
    void update_world(const Memory& mem, uintptr_t client_base);

    // Render thread: fresh view matrix every frame, projects the snapshot.
    std::vector<PlayerESP> project(const Memory& mem, uintptr_t client_base,
                                   int screen_w, int screen_h);

    // Extra draw helpers (render thread only).
    static void draw_head_dot(ImDrawList* dl, const PlayerESP& p, ImU32 col);
    static void draw_skeleton(ImDrawList* dl, const PlayerESP& p, ImU32 col, float thickness);

private:
    std::mutex              world_mutex_;
    std::vector<PlayerData> world_players_;
};
