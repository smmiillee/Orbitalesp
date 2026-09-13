#pragma once
#include <vector>
#include <mutex>
#include <cstdint>
#include "memory.h"
#include "offsets.h"

// Vector types
struct Vec3 {
    float x, y, z;
    Vec3() : x(0), y(0), z(0) {}
    Vec3(float x, float y, float z) : x(x), y(y), z(z) {}
};

struct Vec2 {
    float x, y;
    Vec2() : x(0), y(0) {}
    Vec2(float x, float y) : x(x), y(y) {}
};

struct Matrix4x4 {
    float m[4][4];
    Matrix4x4() { for(int i=0; i<4; i++) for(int j=0; j<4; j++) m[i][j] = 0.0f; }
};

// Player data stored in world thread
struct PlayerData {
    Vec3 origin;
    Vec3 head;
    int health;
    int team;
    float distance;
    bool valid = true;
};

// ESP rendering data per player
struct PlayerESP {
    Vec2 head, feet;
    int health;
    int team;
    float distance;
    float box_h, box_w;
    std::vector<std::pair<int, Vec2>> bones; // bone index -> screen pos
    bool draw_skeleton = true;
    bool draw_head_dots = true;
    bool draw_corners = true;
};

// ESP filtering flags
struct ESPFlags {
    bool bhop = false;
};

class ESP {
private:
    std::vector<PlayerData> world_players;
    std::vector<std::vector<PlayerESP>> player_snapshots;
    std::mutex world_mutex;
    std::mutex render_mutex;
    ESPFlags flags;

    // Core projection
    static bool WorldToScreen(const Vec3& world, Vec2& screen, const Matrix4x4& vm, int w, int h);
    
    // Drawing helpers
    static void DrawSkeleton(ImDrawList* dl, const PlayerESP& p, int sw, int sh, const ImVec4& color);
    static void DrawHeadDots(ImDrawList* dl, const PlayerESP& p, int sw, int sh, const ImVec4& alpha);
    static void DrawCornerBox(ImDrawList* dl, const PlayerESP& p, const ImVec4& color);
    static float CalcDistanceScaled(float dist);

public:
    ESP() = default;

    // Thread functions
    void Thread_Render();

    // Public API
    void UpdateWorld(const Memory& mem, uintptr_t client_base);
    std::vector<PlayerESP> GetProjected(const Memory& mem, uintptr_t client_base, int screen_w, int screen_h, float fov);
    
    // Feature toggles
    void SetBhop(bool enable) { flags.bhop = enable; }
    bool IsBhopEnabled() const { return flags.bhop; }
};
