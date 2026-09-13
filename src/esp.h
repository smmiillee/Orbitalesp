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

// One real frame from the game, timestamped. The render thread interpolates
// between these, which is what removes the shake.
struct Snap {
    double t = 0.0;
    Vec3   origin{}, head{};
    std::array<Vec3, BONE_COUNT> bones{};
    std::array<bool, BONE_COUNT> bone_ok{};
    bool   has_bones = false;
};

struct Track {
    static constexpr int kHist = 24;   // ~190 ms at an 8 ms sample rate

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
    bool  active = false;      // a PLANTED C4 is in the world
    Vec2  screen{};
    Vec2  screen_top{};
    float distance = 0.0f;
};

class ESP {
public:
    // Snapshot interpolation delay, ms. CS2 writes positions at ~64 Hz; drawing
    // those raw steps at 144+ fps is the shake. The render thread draws a fixed
    // time in the past, interpolating between two REAL frames, so the latency
    // is constant -- constant lag is invisible where variable lag reads as
    // shake.
    float interp_delay_ms = 35.0f;

    std::mutex mtx;   // guards tracks_ / bomb state

    void update_world(const Memory& mem, uintptr_t client_base);
    std::vector<PlayerESP> project(const Memory& mem, uintptr_t client_base,
                                   int screen_w, int screen_h);
    BombESP project_bomb(const Memory& mem, uintptr_t client_base,
                         int screen_w, int screen_h);

    int players_alive = 0;
    int  diag_slots   = 0;
    int  diag_carrier = 0;   // 0 = not found, else the carrier's pawn
    int  diag_defidx  = 0;   // local player's active weapon def index

    static bool world_to_screen(const Vec3& world, Vec2& screen,
                                const Matrix4x4& vm, int screen_w, int screen_h);

private:
    struct Calib {
        uintptr_t chunk_off   = offsets::kChunkOff;
        uintptr_t slot_stride = offsets::kSlotStride;

        uintptr_t bone_node = 0, bone_arr = 0;
        bool      bones_ok = false;
        int       bone_fail = 0, probe_cd = 0;

        // Item-definition chain self-test (radar's ValidateDefIdxChain).
        bool defidx_ok = false;
        bool defidx_checked = false;

        // Cached C4 entity so the full scan isn't repeated every frame.
        int  c4_chunk = -1;
        int  c4_slot  = -1;

        // Discovered C4 owner handle offset (radar's DiscoverCarrier).
        uintptr_t carrier_off = 0;
        bool      carrier_ok = false;
        double    last_discover = -1e9;
    } c_;

    std::vector<Track> tracks_;
    bool  bomb_active_ = false;
    Vec3  bomb_origin_{};
};
