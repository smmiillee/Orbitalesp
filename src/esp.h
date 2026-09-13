// --- src/esp.h ---
#pragma once
#include <array>
#include <cstdint>
#include <mutex>
#include <unordered_map>
#include <vector>

#include "memory.h"
#include "offsets.h"

struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };
struct Matrix4x4 { float m[4][4]; };

// ── CS2 skeleton bone ids ────────────────────────────────────────────────
// The two hand bones are the only ones that drift between dumps -- if the
// hands look off by one, try 10 and 15.
enum BoneId : int {
    BONE_PELVIS     = 0,
    BONE_SPINE_1    = 2,
    BONE_SPINE      = 4,
    BONE_NECK       = 5,
    BONE_HEAD       = 6,
    BONE_L_SHOULDER = 8,
    BONE_L_ARM      = 9,
    BONE_L_HAND     = 11,
    BONE_R_SHOULDER = 13,
    BONE_R_ARM      = 14,
    BONE_R_HAND     = 16,
    BONE_L_HIP      = 22,
    BONE_L_KNEE     = 23,
    BONE_L_FOOT     = 24,
    BONE_R_HIP      = 25,
    BONE_R_KNEE     = 26,
    BONE_R_FOOT     = 27,
    BONE_COUNT      = 28,
};

// World-space data, filled by the reader thread.
struct PlayerData {
    uintptr_t pawn = 0;
    Vec3  origin{};                                  // feet
    Vec3  head{};                                    // bone 6 (or origin+70 fallback)
    std::array<Vec3, BONE_COUNT> bones{};
    std::array<bool, BONE_COUNT> bone_ok{};
    bool  has_bones = false;
    int   health = 0;
    int   team = 0;
    float distance = 0.0f;
};

// Screen-space result, recomputed fresh every render frame.
struct PlayerESP {
    Vec2  screen_head{};   // the head BONE (head dot goes here)
    Vec2  screen_top{};    // box top = head bone + a little headroom
    Vec2  screen_feet{};
    std::array<Vec2, BONE_COUNT> bones{};
    std::array<bool, BONE_COUNT> bone_ok{};
    bool  has_bones = false;
    int   health = 0;
    int   team = 0;
    float distance = 0.0f;
    float box_h = 0.0f;
    float box_w = 0.0f;
};

class ESP {
public:
    // Iterpolation window in ms. Only affects PLAYER motion -- the camera is
    // always exact, because the view matrix is read fresh every frame. Set to
    // 0 to disable. This only does anything if the overlay is actually running
    // faster than the game's 64 Hz entity updates.
    float smoothing_ms = 20.0f;

    // Manual alignment. Left in as a last resort, but the resolution is now
    // auto-detected every frame, so these should stay at the defaults.
    float align_scale    = 1.0f;
    float align_offset_x = 0.0f;
    float align_offset_y = 0.0f;

    // World-space data updated by the reader thread.
    std::vector<PlayerData> world_players;
    std::mutex world_mutex;

    void update_world(const Memory& mem, uintptr_t client_base);

    std::vector<PlayerESP> project(const Memory& mem, uintptr_t client_base,
                                   int screen_w, int screen_h);

    static bool world_to_screen(const Vec3& world, Vec2& screen,
                                const Matrix4x4& vm, int screen_w, int screen_h);

    // ── diagnostics (read by the menu) ────────────────────────────────────
    int       last_skeleton_count = 0;  // players with a skeleton drawn
    uintptr_t dbg_node_off  = 0;        // detected C_BaseEntity -> scene node
    uintptr_t dbg_array_off = 0;        // detected scene node -> bone array
    int       dbg_state     = 0;        // 0 = scanning, 1 = locked

private:
    void align(Vec2& s, int screen_w, int screen_h) const;

    // Render-thread-only interpolation state.
    struct Smooth {
        Vec3  origin{};
        Vec3  head{};
        std::array<Vec3, BONE_COUNT> bones{};
        bool  init = false;
        uint64_t last_frame = 0;
    };
    std::unordered_map<uintptr_t, Smooth> smooth_;
};
