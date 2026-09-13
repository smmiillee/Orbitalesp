// --- src/esp.cpp ---
#include "esp.h"

#include <chrono>
#include <cmath>

namespace {

// One entry in the CS2 bone array: Vec3 position + padding out to 0x20 bytes.
struct BoneEntry {
    Vec3  pos;
    float pad[5];
};
static_assert(sizeof(BoneEntry) == offsets::m_boneStride,
              "CS2 bone stride must be 0x20");

// Reads all 28 bone positions in a single bulk read: one syscall per player
// and a guaranteed internally-consistent snapshot (no torn reads mid-update).
bool read_bones(const Memory& mem, uintptr_t pawn,
                std::array<Vec3, BONE_COUNT>& out,
                std::array<bool, BONE_COUNT>& ok) {
    out.fill(Vec3{});
    ok.fill(false);
    if (!pawn) return false;

    const uintptr_t node = mem.read<uintptr_t>(pawn + offsets::m_pGameSceneNode);
    if (!node || node < 0x10000) return false;

    const uintptr_t array =
        mem.read<uintptr_t>(node + offsets::m_modelState + offsets::m_boneArray);
    if (!array || array < 0x10000) return false;

    BoneEntry raw[BONE_COUNT];
    if (!mem.read_bytes(array, raw, sizeof(raw))) return false;

    int valid = 0;
    for (int i = 0; i < BONE_COUNT; ++i) {
        const Vec3 p = raw[i].pos;
        const bool sane =
            std::isfinite(p.x) && std::isfinite(p.y) && std::isfinite(p.z) &&
            std::fabs(p.x) < 100000.0f &&
            std::fabs(p.y) < 100000.0f &&
            std::fabs(p.z) < 100000.0f &&
            !(p.x == 0.0f && p.y == 0.0f && p.z == 0.0f);
        if (!sane) continue;
        out[i] = p;
        ok[i]  = true;
        ++valid;
    }

    // Need at least a plausible spine/head before we draw anything.
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

    // Local player position, so distance is measured from YOU and not from
    // the map origin (the old code used sqrt(x^2+y^2+z^2)).
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

    std::vector<PlayerData> fresh;

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

            PlayerData pd;
            pd.pawn   = entity;
            pd.origin = origin;
            pd.health = health;
            pd.team   = team;

            const float dx = origin.x - local_origin.x;
            const float dy = origin.y - local_origin.y;
            const float dz = origin.z - local_origin.z;
            pd.distance = std::sqrt(dx * dx + dy * dy + dz * dz) / 40.0f;

            pd.has_bones = read_bones(mem, entity, pd.bones, pd.bone_ok);
            pd.head = pd.has_bones
                        ? pd.bones[BONE_HEAD]
                        : Vec3{ origin.x, origin.y, origin.z + 70.0f };

            fresh.push_back(pd);
        }
    }

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
    // CS2 only writes entity positions at ~64 Hz. Sampling faster (your
    // 4 ms thread) just reads the SAME value several times and then steps,
    // so a raw value handed to a 144/240 Hz overlay is a visible staircase.
    // Smoothing the position in the time domain -- frame-rate independent --
    // is what actually removes it.
    static auto last = std::chrono::steady_clock::now();
    const auto now_tp = std::chrono::steady_clock::now();

    float dt = std::chrono::duration<float>(now_tp - last).count();
    last = now_tp;
    if (dt < 0.0f)  dt = 0.0f;
    if (dt > 0.10f) dt = 0.10f;

    static uint64_t frame = 0;
    ++frame;

    constexpr float kTau  = 0.045f;                       // ~45 ms window
    const float alpha = 1.0f - std::exp(-dt / kTau);       // clamped, 0..1

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

        PlayerESP e;
        if (!world_to_screen(s.head,   e.screen_head, vm, screen_w, screen_h)) continue;
        if (!world_to_screen(s.origin, e.screen_feet, vm, screen_w, screen_h)) continue;

        e.box_h = e.screen_feet.y - e.screen_head.y;
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
                if (world_to_screen(s.bones[i], sp, vm, screen_w, screen_h)) {
                    e.bones[i]   = sp;
                    e.bone_ok[i] = true;
                }
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
