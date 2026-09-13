// --- src/esp.cpp ---
#include "esp.h"

#include <chrono>
#include <cmath>
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

// The bone chain is NOT stable across updates:
//   pawn + sceneNodeOff  -> CGameSceneNode
//   node + boneArrayOff  -> CBoneData[28]
// sceneNodeOff has shipped as 0x310/0x328/0x330/0x338 and boneArrayOff
// (= m_modelState + 0x80) as 0x1C0/0x1E0/0x210. So BOTH are searched for at
// runtime instead of guessed -- that is why the skeleton never drew before.
struct BoneChain {
    uintptr_t scene_node_off = 0;
    uintptr_t bone_array_off = 0;
    bool valid = false;
};

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

// Is this a believable player skeleton?
//
// Deliberately mostly SELF-relative: head above neck above pelvis, arms-length
// torso span, player-sized proportions. The origin is only used as a loose
// sanity bound, so a m_vOldOrigin that isn't exactly at the feet can't make
// detection fail.
bool geometry_ok(const Vec3& pelvis, const Vec3& neck, const Vec3& head,
                 const Vec3& origin) {
    if (!sane_pos(pelvis) || !sane_pos(neck) || !sane_pos(head)) return false;

    if (!(head.z > neck.z && neck.z > pelvis.z)) return false;

    const float span = head.z - pelvis.z;      // ~26 units on a real player
    if (span < 15.0f || span > 60.0f) return false;

    const float dx = head.x - pelvis.x;
    const float dy = head.y - pelvis.y;
    if (dx * dx + dy * dy > 40.0f * 40.0f) return false;

    // Loose check that the skeleton belongs to THIS entity.
    if (std::fabs(head.x - origin.x) > 150.0f) return false;
    if (std::fabs(head.y - origin.y) > 150.0f) return false;
    if (std::fabs(head.z - origin.z) > 150.0f) return false;
    if (std::fabs(pelvis.z - origin.z) > 150.0f) return false;

    return true;
}

struct TestPawn {
    uintptr_t pawn;
    Vec3      origin;
};

BoneChain probe_bone_chain(const Memory& mem, const std::vector<TestPawn>& test) {
    BoneChain best;
    int best_score = 0;
    const int need = (test.size() >= 2) ? 2 : 1;
    if (test.empty()) return best;

    static constexpr size_t kNodeQwords = 0x400 / 8;
    uint64_t fields[kNodeQwords];

    for (uintptr_t node_off = offsets::kSceneNodeScanBegin;
         node_off <= offsets::kSceneNodeScanEnd;
         node_off += offsets::kScanStep) {

        const uintptr_t node = mem.read<uintptr_t>(test[0].pawn + node_off);
        if (!valid_ptr(node)) continue;

        // ONE bulk read of the whole scene node. The inner loop then runs
        // entirely in-process, which is what makes a two-level search cheap
        // enough to run at all.
        if (!mem.read_bytes(node, fields, sizeof(fields))) continue;

        for (uintptr_t array_off = offsets::kBoneArrayScanBegin;
             array_off <= offsets::kBoneArrayScanEnd;
             array_off += 8) {

            if (!valid_ptr(fields[array_off / 8])) continue;

            int score = 0;
            for (size_t t = 0; t < test.size(); ++t) {
                uintptr_t arr;
                if (t == 0) {
                    arr = fields[array_off / 8];
                } else {
                    const uintptr_t n = mem.read<uintptr_t>(test[t].pawn + node_off);
                    if (!valid_ptr(n)) break;
                    arr = mem.read<uintptr_t>(n + array_off);
                    if (!valid_ptr(arr)) break;
                }

                BoneEntry b[7]; // bones 0..6 covers pelvis, neck and head
                if (!mem.read_bytes(arr, b, sizeof(b))) break;
                if (!geometry_ok(b[BONE_PELVIS].pos, b[BONE_NECK].pos,
                                 b[BONE_HEAD].pos, test[t].origin)) break;
                ++score;
            }

            if (score > best_score) {
                best_score = score;
                best.scene_node_off = node_off;
                best.bone_array_off = array_off;
            }
        }
    }

    best.valid = best_score >= need;
    return best;
}

// One bulk read for all 28 bones: one syscall per player, and a snapshot that
// can't be torn mid-update.
bool read_bones(const Memory& mem, uintptr_t pawn, const BoneChain& chain,
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

    int valid = 0;
    for (int i = 0; i < BONE_COUNT; ++i) {
        if (!sane_pos(raw[i].pos)) continue;
        out[i] = raw[i].pos;
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
    // Throttled: a failed probe costs a few ms, so retrying it every 4 ms
    // would starve the reader thread (and it is what made the ESP hitch).
    static BoneChain s_chain{};
    static int s_bone_fail    = 0;
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

            const BoneChain found = probe_bone_chain(mem, test);
            if (found.valid) {
                s_chain = found;
                s_bone_fail = 0;
                dbg_node_off  = found.scene_node_off;
                dbg_array_off = found.bone_array_off;
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
            pd.has_bones = read_bones(mem, r.pawn, s_chain, pd.bones, pd.bone_ok);
        if (pd.has_bones) ++bone_hits;

        pd.head = pd.has_bones
                    ? pd.bones[BONE_HEAD]
                    : Vec3{ r.origin.x, r.origin.y, r.origin.z + 70.0f };

        fresh.push_back(pd);
    }

    // Losing every skeleton while players are visible means the offsets moved
    // (game updated) -> force a re-probe.
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

    // ── interpolation ─────────────────────────────────────────────────────
    // CS2 writes entity positions at ~64 Hz. This only has a visible effect
    // when the overlay actually renders faster than that -- see the spin-wait
    // in main.cpp, because plain sleep_for() on Windows silences it.
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
                // Teleport / respawn / entity reuse -> snap, never smear.
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

    // Drop interpolation state for players that are gone (~400 frames).
    for (auto it = smooth_.begin(); it != smooth_.end(); ) {
        if (frame - it->second.last_frame > 400) it = smooth_.erase(it);
        else                                     ++it;
    }

    return result;
}
