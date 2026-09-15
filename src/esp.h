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

enum BoneId : int {
    BONE_ORIGIN = 0, BONE_PELVIS = 1, BONE_SPINE_0 = 2, BONE_SPINE_1 = 3,
    BONE_SPINE_2 = 4, BONE_NECK = 6, BONE_HEAD = 7, BONE_CLAVICLE_L = 8,
    BONE_L_SHOULDER = 9, BONE_L_ELBOW = 10, BONE_L_HAND = 11,
    BONE_CLAVICLE_R = 12, BONE_R_SHOULDER = 13, BONE_R_ELBOW = 14,
    BONE_R_HAND = 15, BONE_L_HIP = 17, BONE_L_KNEE = 18, BONE_L_FOOT = 19,
    BONE_R_HIP = 20, BONE_R_KNEE = 21, BONE_R_FOOT = 22, BONE_CHEST = 23,
    BONE_GUN = 24, BONE_EYE_L = 25, BONE_EYE_R = 26, BONE_COUNT = 32,
};

// ---------------------------------------------------------------------------
// THE WIREFRAME MESH
//
// These segments ARE the hitbox skeleton: CS2 hitboxes are capsules laid out
// around the bone chain, and each capsule's axis is the segment between two
// bones. So this table doubles as
//   * the skeleton we DRAW, and
//   * the mesh the TRIGGERBOT tests against.
//
// Testing distance to a SEGMENT instead of to a joint is the whole point: it
// covers the limb between two bones, so the crosshair no longer has to land
// exactly on a joint to count as a hit.
//
// inline constexpr gives one definition across translation units, so esp.cpp
// and aim.cpp share this table rather than each keeping a copy that could drift.
// ---------------------------------------------------------------------------
struct BoneLink { int a, b; };

inline constexpr BoneLink kBoneLinks[] = {
    { BONE_PELVIS,     BONE_SPINE_1    },
    { BONE_SPINE_1,    BONE_SPINE_2    },
    { BONE_SPINE_2,    BONE_CHEST      },
    { BONE_CHEST,      BONE_NECK       },
    { BONE_NECK,       BONE_HEAD       },
    { BONE_NECK,       BONE_L_SHOULDER },
    { BONE_L_SHOULDER, BONE_L_ELBOW    },
    { BONE_L_ELBOW,    BONE_L_HAND     },
    { BONE_NECK,       BONE_R_SHOULDER },
    { BONE_R_SHOULDER, BONE_R_ELBOW    },
    { BONE_R_ELBOW,    BONE_R_HAND     },
    { BONE_PELVIS,     BONE_L_HIP      },
    { BONE_L_HIP,      BONE_L_KNEE     },
    { BONE_L_KNEE,     BONE_L_FOOT     },
    { BONE_PELVIS,     BONE_R_HIP      },
    { BONE_R_HIP,      BONE_R_KNEE     },
    { BONE_R_KNEE,     BONE_R_FOOT     },
};
inline constexpr int kBoneLinkCount =
    static_cast<int>(sizeof(kBoneLinks) / sizeof(kBoneLinks[0]));

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
    int  count = 0, head = 0;
    void push(const Snap& s) {
        hist[head] = s;
        head = (head + 1) % kHist;
        if (count < kHist) ++count;
    }
};

struct PlayerESP {
    uintptr_t pawn = 0;   // needed by the triggerbot's vis check
    Vec2  screen_head{}, screen_top{}, screen_feet{};
    std::array<Vec2, BONE_COUNT> bones{};
    std::array<bool, BONE_COUNT> bone_ok{};
    bool  has_bones = false;
    bool  has_bomb = false;
    int   health = 0, team = 0;
    float distance = 0.0f, box_h = 0.0f, box_w = 0.0f;
    char  name[32]{};
    char  weapon[24]{};
};

struct BombESP {
    bool  active = false;
    Vec2  screen{}, screen_top{};
    float distance = 0.0f;
};

class ESP {
public:
    float interp_delay_ms = 35.0f;
    std::mutex mtx;

    void update_world(const Memory& mem, uintptr_t client_base);

    std::vector<PlayerESP> project(const Memory& mem, uintptr_t client_base,
                                   int screen_w, int screen_h,
                                   bool interpolate = true);
    BombESP project_bomb(const Memory& mem, uintptr_t client_base,
                         int screen_w, int screen_h);

    int players_alive = 0;

    bool      diag_bones_ok = false;
    uintptr_t diag_bone_node = 0, diag_bone_arr = 0;
    bool      diag_wsvc_ok = false;
    uintptr_t diag_wsvc = 0;
    int       diag_defidx = 0;
    int       diag_defidx_n = 0;   // how many offsets are known
    uintptr_t diag_c4_ent = 0;

    static bool world_to_screen(const Vec3& world, Vec2& screen,
                                const Matrix4x4& vm, int screen_w, int screen_h);

private:
    struct C4Cand { bool on_node = true; uintptr_t off = 0; };

    struct Calib {
        uintptr_t chunk_off = offsets::kChunkOff;
        uintptr_t slot_stride = offsets::kSlotStride;

        uintptr_t bone_node = 0, bone_arr = 0;
        bool bones_ok = false;
        int  bone_fail = 0, probe_cd = 0;

        uintptr_t wsvc = 0;
        bool      wsvc_ok = false;
        double    wsvc_try = 0.0;

        double defidx_try = -1e9;

        C4Cand c4_cand{};
        bool   c4_ok = false;
        Vec3   c4_last{};
        double c4_last_t = 0.0;
        int    c4_stable = 0;
    } c_;

    std::vector<Track> tracks_;
    bool  bomb_active_ = false;
    Vec3  bomb_origin_{};
};

// Def index of the local player's ACTIVE weapon, 0 if unknown. The triggerbot
// uses it to pick a hardcoded per-weapon shot interval.
int ESP_LocalWeaponId(const Memory& mem, uintptr_t client_base);
