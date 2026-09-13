// --- src/esp.cpp ---
#include "esp.h"

#include <chrono>
#include <cmath>
#include <cstring>
#include <utility>

namespace {

// One entry in the CS2 bone array: Vec3 position + padding out to the stride.
struct BoneEntry {
    Vec3  pos;
    float pad[5];
};
static_assert(sizeof(BoneEntry) == offsets::m_boneStride,
              "CS2 bone stride must be 0x20");

// Box top sits a few units above the head bone so the skull isn't clipped.
// The head DOT uses the bone itself.
constexpr float kHeadPad = 6.0f;

// Height of the head bone above the feet, used ONLY as the fallback when the
// real bones can't be read.
constexpr float kFallbackHeadZ = 64.0f;

// The bone chain is NOT stable across updates, and neither is the offset of
// the scene-node pointer inside the pawn. So nothing here is assumed: the probe
// scans every pointer inside the pawn, then every pointer inside that scene
// node, and keeps whichever pair yields an actual skeleton.
struct BoneChain {
    uintptr_t scene_node_off = 0;
    uintptr_t bone_array_off = 0;
    bool valid = false;
};

constexpr size_t kPawnScanBytes = 0x900;  // covers m_pGameSceneNode and friends
constexpr size_t kNodeScanBytes = 0x800;  // covers CSkeletonInstance/CModelState

bool valid_ptr(uintptr_t p) {
    return p > 0x10000 && p < 0x00007FFFFFFF0000ULL && (p % 8) == 0;
}

bool sane_pos(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
           std::fabs(v.x) < 100000.0f &&
           std::fabs(v.y) < 100000.0f &&
           std::fabs(v.z) < 100000.0f &&
           !(v.x == 0.0f && v.y == 0.0f && v.z == 0.0f);
}

// ── the detector ──────────────────────────────────────────────────────────
// Deliberately INDEX-AGNOSTIC. It does not care which bone is the head or
// whether the ordering is what we expect -- it only asks "is this a dense
// cloud of sane points sitting inside one player's bounding box?".
//
// The index map itself is NOT checked here, and cannot be: a correct array
// with stale bone numbers passes this test perfectly, which is exactly how a
// garbled skeleton got drawn while the chain read "0x330/0x240". The numbering
// lives in esp.h instead.
int skeleton_point_count(const Memory& mem, uintptr_t arr, const Vec3& origin) {
    BoneEntry b[BONE_COUNT];
    if (!mem.read_bytes(arr, b, sizeof(b))) return 0;

    int good = 0;
    for (int i = 0; i < BONE_COUNT; ++i) {
        const Vec3& p = b[i].pos;
        if (!sane_pos(p)) continue;

        const float dx = p.x - origin.x;
        const float dy = p.y - origin.y;
        const float dz = p.z - origin.z;

        // A player is ~72 units tall and a couple of feet wide; a bone cloud
        // belonging to someone else, or to nothing at all, won't fit in here.
        if (std::fabs(dx) > 110.0f) continue;
        if (std::fabs(dy) > 110.0f) continue;
        if (dz < -40.0f || dz > 150.0f) continue;

        ++good;
    }
    return good;
}

struct TestPawn {
    uintptr_t pawn;
    Vec3      origin;
};

BoneChain probe_bone_chain(const Memory& mem, const std::vector<TestPawn>& test,
                           int* pts_out) {
    BoneChain best;
    int best_score = 0;
    int best_pts   = 0;
    const int need = (test.size() >= 2) ? 2 : 1;
    if (test.empty()) return best;

    uint8_t node_buf[kNodeScanBytes];

    for (uintptr_t node_off = 0x8; node_off + 8 <= kPawnScanBytes; node_off += 8) {
        const uintptr_t node = mem.read<uintptr_t>(test[0].pawn + node_off);
        if (!valid_ptr(node)) continue;

        // ONE bulk read of the scene node; everything below runs in-process.
        if (!mem.read_bytes(node, node_buf, sizeof(node_buf))) continue;

        for (uintptr_t array_off = 0x8;
             array_off + 8 <= kNodeScanBytes;
             array_off += 8) {

            uintptr_t arr = 0;
            std::memcpy(&arr, node_buf + array_off, sizeof(arr));
            if (!valid_ptr(arr)) continue;

            const int pts = skeleton_point_count(mem, arr, test[0].origin);
            if (pts < 20) continue;   // most of the skeleton has to be there

            // Confirm on the other players before believing it.
            int score = 1;
            for (size_t t = 1; t < test.size(); ++t) {
                const uintptr_t n =
                    mem.read<uintptr_t>(test[t].pawn + node_off);
                if (!valid_ptr(n)) break;

                const uintptr_t a = mem.read<uintptr_t>(n + array_off);
                if (!valid_ptr(a)) break;

                if (skeleton_point_count(mem, a, test[t].origin) < 20) break;
                ++score;
            }

            if (score > best_score || (score == best_score && pts > best_pts)) {
                best_score = score;
                best_pts   = pts;
                best.scene_node_off = node_off;
                best.bone_array_off = array_off;
            }
        }
    }

    if (pts_out) *pts_out = best_pts;
    best.valid = best_score >= need;
    return best;
}

// One bulk read for all bones: one syscall per player, and a snapshot that
// can't be torn mid-update.
bool read_bones(const Memory& mem, uintptr_t pawn, const Vec3& origin,
                const BoneChain& chain,
                std::array<Vec3, BONE_COUNT>& out,
                std::array<bool, BONE_COUNT>& ok) {
    out.fill(Vec3{});
    ok.fill(false);
    if (!chain.valid || !pawn) return false;

    const uintptr_t node = mem.read<uintptr_t>(pawn + chain.scene_node_off);
    if (!valid_ptr(node)) return false;

    const uintptr_t arr = mem.read<uintptr_t>(node + chain.bone_array_off);
    if (!valid_ptr(arr)) return false;

    BoneEntry raw[BONE_COUNT];
    if (!mem.read_bytes(arr, raw, sizeof(raw))) return false;

    // Per-bone sanity, measured against the player's own feet. This is what
    // stops a bone that maps to the wrong body part (or a stray model bone far
    // from the body) from being drawn as a line across the map -- it is simply
    // skipped instead. Tighter than the probe's box, since by now we know
    // exactly which entity these bones belong to.
    int valid = 0;
    for (int i = 0; i < BONE_COUNT; ++i) {
        const Vec3& p = raw[i].pos;
        if (!sane_pos(p)) continue;

        const float dx = p.x - origin.x;
        const float dy = p.y - origin.y;
        const float dz = p.z - origin.z;
        if (std::fabs(dx) > 60.0f || std::fabs(dy) > 60.0f) continue;
        if (dz < -25.0f || dz > 90.0f) continue;

        out[i] = p;
        ok[i]  = true;
        ++valid;
    }
    return valid >= 6;
}

// Unchanged from your working build: finds the entity-list chunk offset/stride.
void calibrate(const Memory& mem, uintptr_t entity_list, uintptr_t local_pawn,
               uintptr_t& best_off, uintptr_t& best_stride) {
    struct Combo { uintptr_t off; uintptr_t stride; };
    constexpr Combo combos[] = {
        {0x10, 112}, {0x10, 120}, {0x08, 112}, {0x08, 120}
    };

    int best_score = -1;
    best_off    = 0x10;
    best_stride = 120;

    for (const auto& c : combos) {
        int score = 0;
        for (int chunk = 0; chunk < 4; ++chunk) {
            const uintptr_t cp = mem.read<uintptr_t>(entity_list + c.off + 8 * chunk);
            if (!cp || cp < 0x10000) continue;

            for (int i = 0; i < 512; ++i) {
                const uintptr_t ent = mem.read<uintptr_t>(cp + c.stride * i);
                if (!ent || ent < 0x10000 || ent == local_pawn) continue;

                const int hp = mem.read<int>(ent + offsets::m_iHealth);
                if (hp <= 0 || hp > 100) continue;

                const int tm = mem.read<int>(ent + offsets::m_iTeamNum) & 0xFF;
                if (tm == 2 || tm == 3) ++score;
            }
        }
        if (score > best_score) {
            best_score  = score;
            best_off    = c.off;
            best_stride = c.stride;
        }
    }
}

} // namespace

bool ESP::world_to_screen(const Vec3& world, Vec2& screen,
                          const Matrix4x4& vm, int screen_w, int screen_h) {
    const float w = vm.m[3][0] * world.x + vm.m[3][1] * world.y
                  + vm.m[3][2] * world.z + vm.m[3][3];
    if (w < 0.001f) return false;

    const float x = vm.m[0][0] * world.x + vm.m[0][1] * world.y
                  + vm.m[0][2] * world.z + vm.m[0][3];
    const float y = vm.m[1][0] * world.x + vm.m[1][1] * world.y
                  + vm.m[1][2] * world.z + vm.m[1][3];

    const float inv_w = 1.0f / w;
    screen.x = (screen_w * 0.5f) + (x * inv_w) * (screen_w * 0.5f);
    screen.y = (screen_h * 0.5f) - (y * inv_w) * (screen_h * 0.5f);
    return true;
}

void ESP::align(Vec2& s, int screen_w, int screen_h) const {
    if (align_scale == 1.0f && align_offset_x == 0.0f && align_offset_y == 0.0f)
        return;

    const float cx = screen_w * 0.5f;
    const float cy = screen_h * 0.5f;
    s.x = cx + (s.x - cx) * align_scale + align_offset_x;
    s.y = cy + (s.y - cy) * align_scale + align_offset_y;
}

// Reader thread: world positions + bones only. No view matrix here.
void ESP::update_world(const Memory& mem, uintptr_t client_base) {
    const uintptr_t entity_list =
        mem.read<uintptr_t>(client_base + offsets::dwEntityList);
    const uintptr_t local_pawn =
        mem.read<uintptr_t>(client_base + offsets::dwLocalPlayerPawn);

    if (!entity_list || !local_pawn) {
        std::lock_guard<std::mutex> lock(world_mutex);
        world_players.clear();
        return;
    }

    Vec3 local_origin{};
    local_origin.x = mem.read<float>(local_pawn + offsets::m_vOldOrigin);
    local_origin.y = mem.read<float>(local_pawn + offsets::m_vOldOrigin + 4);
    local_origin.z = mem.read<float>(local_pawn + offsets::m_vOldOrigin + 8);

    static uintptr_t s_off = 0, s_stride = 0;
    static bool s_done = false;
    if (!s_done) {
        calibrate(mem, entity_list, local_pawn, s_off, s_stride);
        s_done = true;
    }

    // ── pass 1: collect live pawns ────────────────────────────────────────
    struct Raw {
        uintptr_t pawn;
        Vec3      origin;
        int       health;
        int       team;
        float     distance;
    };
    std::vector<Raw> raw;

    for (int chunk = 0; chunk < 4; ++chunk) {
        const uintptr_t chunk_ptr =
            mem.read<uintptr_t>(entity_list + s_off + 8 * chunk);
        if (!chunk_ptr || chunk_ptr < 0x10000) continue;

        for (int i = 0; i < 512; ++i) {
            const uintptr_t entity = mem.read<uintptr_t>(chunk_ptr + s_stride * i);
            if (!entity || entity < 0x10000 || entity == local_pawn) continue;

            const int health = mem.read<int>(entity + offsets::m_iHealth);
            if (health <= 0 || health > 100) continue;

            const int team = mem.read<int>(entity + offsets::m_iTeamNum) & 0xFF;
            if (team != 2 && team != 3) continue;

            Vec3 origin{};
            origin.x = mem.read<float>(entity + offsets::m_vOldOrigin);
            origin.y = mem.read<float>(entity + offsets::m_vOldOrigin + 4);
            origin.z = mem.read<float>(entity + offsets::m_vOldOrigin + 8);
            if (origin.x == 0.0f && origin.y == 0.0f) continue;

            const float dx = origin.x - local_origin.x;
            const float dy = origin.y - local_origin.y;
            const float dz = origin.z - local_origin.z;
            raw.push_back({ entity, origin, health, team,
                            std::sqrt(dx * dx + dy * dy + dz * dz) / 40.0f });
        }
    }

    // ── pass 2: resolve the bone chain ────────────────────────────────────
    static BoneChain s_chain{};
    static int s_bone_fail      = 0;
    static int s_probe_cooldown = 0;

    if (!s_chain.valid || s_bone_fail > 250) {
        if (s_probe_cooldown > 0) {
            --s_probe_cooldown;
        } else {
            std::vector<TestPawn> test;
            if (local_origin.x != 0.0f || local_origin.y != 0.0f)
                test.push_back({ local_pawn, local_origin });
            for (const auto& r : raw) {
                if (test.size() >= 4) break;
                test.push_back({ r.pawn, r.origin });
            }

            int pts = 0;
            const BoneChain found = probe_bone_chain(mem, test, &pts);
            if (found.valid) {
                s_chain = found;
                s_bone_fail = 0;
                dbg_node_off  = found.scene_node_off;
                dbg_array_off = found.bone_array_off;
                dbg_pts       = pts;
                dbg_state     = 1;
            }
            s_probe_cooldown = 375; // ~1.5 s at 4 ms
        }
    }

    // ── pass 3: read bones and publish ────────────────────────────────────
    std::vector<PlayerData> fresh;
    fresh.reserve(raw.size());
    int bone_hits = 0;

    for (const auto& r : raw) {
        PlayerData pd;
        pd.pawn     = r.pawn;
        pd.origin   = r.origin;
        pd.health   = r.health;
        pd.team     = r.team;
        pd.distance = r.distance;

        if (s_chain.valid)
            pd.has_bones = read_bones(mem, r.pawn, r.origin, s_chain,
                                      pd.bones, pd.bone_ok);
        if (pd.has_bones) ++bone_hits;

        pd.head = (pd.has_bones && pd.bone_ok[BONE_HEAD])
                    ? pd.bones[BONE_HEAD]
                    : Vec3{ r.origin.x, r.origin.y, r.origin.z + kFallbackHeadZ };

        fresh.push_back(pd);
    }

    if (s_chain.valid && !raw.empty() && bone_hits == 0) ++s_bone_fail;
    else                                               s_bone_fail = 0;

    std::lock_guard<std::mutex> lock(world_mutex);
    world_players = std::move(fresh);
}

// Render thread: fresh view matrix THIS frame + interpolated world positions.
std::vector<PlayerESP> ESP::project(const Memory& mem, uintptr_t client_base,
                                    int screen_w, int screen_h) {
    const Matrix4x4 vm = mem.read<Matrix4x4>(client_base + offsets::dwViewMatrix);

    std::vector<PlayerData> snapshot;
    {
        std::lock_guard<std::mutex> lock(world_mutex);
        snapshot = world_players;
    }

    static auto last = std::chrono::steady_clock::now();
    const auto now_tp = std::chrono::steady_clock::now();

    float dt = std::chrono::duration<float>(now_tp - last).count();
    last = now_tp;
    if (dt < 0.0f)  dt = 0.0f;
    if (dt > 0.10f) dt = 0.10f;

    static uint64_t frame = 0;
    ++frame;

    const float tau   = smoothing_ms / 1000.0f;
    const float alpha = (tau > 0.0f) ? (1.0f - std::exp(-dt / tau)) : 1.0f;

    auto lerp_to = [alpha](Vec3& a, const Vec3& b) {
        a.x += (b.x - a.x) * alpha;
        a.y += (b.y - a.y) * alpha;
        a.z += (b.z - a.z) * alpha;
    };

    std::vector<PlayerESP> result;

    for (const auto& p : snapshot) {
        Smooth& s = smooth_[p.pawn];
        s.last_frame = frame;

        if (!s.init) {
            s.origin = p.origin;
            s.head   = p.head;
            s.bones  = p.bones;
            s.init   = true;
        } else {
            const float dx = p.origin.x - s.origin.x;
            const float dy = p.origin.y - s.origin.y;
            const float dz = p.origin.z - s.origin.z;

            if (dx * dx + dy * dy + dz * dz > 500.0f * 500.0f) {
                s.origin = p.origin;
                s.head   = p.head;
                s.bones  = p.bones;
            } else {
                lerp_to(s.origin, p.origin);
                lerp_to(s.head,   p.head);
                if (p.has_bones) {
                    for (int i = 0; i < BONE_COUNT; ++i)
                        if (p.bone_ok[i]) lerp_to(s.bones[i], p.bones[i]);
                }
            }
        }

        const Vec3 box_top{ s.head.x, s.head.y, s.head.z + kHeadPad };

        PlayerESP e;
        if (!world_to_screen(s.head,   e.screen_head, vm, screen_w, screen_h)) continue;
        if (!world_to_screen(box_top,  e.screen_top,  vm, screen_w, screen_h)) continue;
        if (!world_to_screen(s.origin, e.screen_feet, vm, screen_w, screen_h)) continue;

        align(e.screen_head, screen_w, screen_h);
        align(e.screen_top,  screen_w, screen_h);
        align(e.screen_feet, screen_w, screen_h);

        e.box_h = e.screen_feet.y - e.screen_top.y;
        if (e.box_h < 5.0f) continue;

        e.box_w     = e.box_h * 0.45f;
        e.health    = p.health;
        e.team      = p.team;
        e.distance  = p.distance;
        e.has_bones = p.has_bones;

        if (p.has_bones) {
            for (int i = 0; i < BONE_COUNT; ++i) {
                if (!p.bone_ok[i]) continue;
                Vec2 sp{};
                if (!world_to_screen(s.bones[i], sp, vm, screen_w, screen_h)) continue;
                align(sp, screen_w, screen_h);
                e.bones[i]   = sp;
                e.bone_ok[i] = true;
            }
        }

        result.push_back(e);
    }

    for (auto it = smooth_.begin(); it != smooth_.end(); ) {
        if (frame - it->second.last_frame > 400) it = smooth_.erase(it);
        else                                     ++it;
    }

    return result;
}
