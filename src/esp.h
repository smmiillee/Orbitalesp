// --- src/esp.h ---
#pragma once
#include <array>
#include <cstdint>
#include <mutex>
#include <vector>

#include "memory.h"
#include "offsets.h"

struct Vec3 { float x, y, z; };
struct Vec2 { float x, y; };
struct Matrix4x4 { float m[4][4]; };

// ── CS2 skeleton bone ids (post animgraph_2_beta) ───────────────────────
enum BoneId : int {
    BONE_ORIGIN     = 0,
    BONE_PELVIS     = 1,
    BONE_SPINE_0    = 2,
    BONE_SPINE_1    = 3,
    BONE_SPINE_2    = 4,
    BONE_NECK       = 6,
    BONE_HEAD       = 7,
    BONE_CLAVICLE_L = 8,
    BONE_L_SHOULDER = 9,
    BONE_L_ELBOW    = 10,
    BONE_L_HAND     = 11,
    BONE_CLAVICLE_R = 12,
    BONE_R_SHOULDER = 13,
    BONE_R_ELBOW    = 14,
    BONE_R_HAND     = 15,
    BONE_L_HIP      = 17,
    BONE_L_KNEE     = 18,
    BONE_L_FOOT     = 19,
    BONE_R_HIP      = 20,
    BONE_R_KNEE     = 21,
    BONE_R_FOOT     = 22,
    BONE_CHEST      = 23,
    BONE_GUN        = 24,
    BONE_EYE_L      = 25,
    BONE_EYE_R      = 26,
    BONE_COUNT      = 32,
};

struct Snap {
    double t = 0.0;
    Vec3   origin{}, head{};
    std::array<Vec3, BONE_COUNT> bones{};
    std::array<bool, BONE_COUNT> bone_ok{};
    bool   has_bones = false;
};

struct Track {
    static constexpr int kHist = 24;

    uintptr_t pawn = 0;
    char  name[32]{};
    char  weapon[24]{};
    bool  has_bomb = false;
    int   health = 0;
    int   team = 0;
    float distance = 0.0f;

    Snap hist[kHist];
    int  count = 0;
    int  head  = 0;

    void push(const Snap& s) {
        hist[head] = s;
        head = (head + 1) % kHist;
        if (count < kHist) ++count;
    }
};

struct PlayerESP {
    Vec2  screen_head{}, screen_top{}, screen_feet{};
    std::array<Vec2, BONE_COUNT> bones{};
    std::array<bool, BONE_COUNT> bone_ok{};
    bool  has_bones = false;
    bool  has_bomb = false;
    int   health = 0;
    int   team = 0;
    float distance = 0.0f;
    float box_h = 0.0f;
    float box_w = 0.0f;
    char  name[32]{};
    char  weapon[24]{};
};

struct BombESP {
    bool  active = false;
    Vec2  screen{};
    Vec2  screen_top{};
    float distance = 0.0f;
};

class ESP {
public:
    float interp_delay_ms = 35.0f;

    std::mutex mtx;

    void update_world(const Memory& mem, uintptr_t client_base);

    // interpolate = true  -> smooth motion for the ESP (default)
    // interpolate = false -> newest sample, no delay. Used by the triggerbot,
    //                        where a 35 ms-stale position would mean firing
    //                        behind a moving target.
    std::vector<PlayerESP> project(const Memory& mem, uintptr_t client_base,
                                   int screen_w, int screen_h,
                                   bool interpolate = true);

    BombESP project_bomb(const Memory& mem, uintptr_t client_base,
                         int screen_w, int screen_h);

    int players_alive = 0;

    static bool world_to_screen(const Vec3& world, Vec2& screen,
                                const Matrix4x4& vm, int screen_w, int screen_h);

private:
    struct Calib {
        uintptr_t chunk_off   = offsets::kChunkOff;
        uintptr_t slot_stride = offsets::kSlotStride;

        uintptr_t bone_node = 0, bone_arr = 0;
        bool      bones_ok = false;
        int       bone_fail = 0, probe_cd = 0;

        // Planted-C4 position: found and then VERIFIED by requiring the Z to
        // stay put, because a planted bomb does not move.
        uintptr_t c4_node_off = 0;
        bool      c4_pos_ok = false;
        Vec3      c4_last{};
        double    c4_last_t = 0.0;
        int       c4_stable = 0;
    } c_;

    std::vector<Track> tracks_;
    bool   bomb_active_ = false;
    Vec3   bomb_origin_{};
};
