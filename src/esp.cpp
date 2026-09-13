// --- src/esp.cpp ---
// Entity scan: the exact iteration the previously WORKING version used
// (list + 0x10 + 8*chunk, stride 0x78, 4 chunks x 512 slots, health/team
// filter). Only additions bolted onto the same entities: bones, and
// distance measured from the local pawn instead of world origin.
#include "esp.h"
#include "offsets.h"
#include "sdk.h"

#include <imgui.h>
#include <cmath>

static constexpr float kHeadFallbackHeight = 70.0f; // dot fallback if bones are unavailable

// One full bone table (ids 0..27) read in a single ReadProcessMemory call.
struct BoneEntry {
    Vec3  pos;
    float pad[5];
};
static_assert(sizeof(BoneEntry) == 32, "CS2 bone entries are 32 bytes");

void ESP::update_world(const Memory& mem, uintptr_t client_base) {
    const uintptr_t entity_list = mem.read<uintptr_t>(client_base + offsets::dwEntityList);
    const uintptr_t local_pawn  = mem.read<uintptr_t>(client_base + offsets::dwLocalPlayerPawn);

    if (!entity_list || !local_pawn) {
        std::lock_guard<std::mutex> lock(world_mutex_);
        world_players_.clear();
        return;
    }

    const Vec3 local_origin = mem.read<Vec3>(local_pawn + offsets::m_vOldOrigin);

    std::vector<PlayerData> fresh;
    fresh.reserve(32);

    for (int chunk = 0; chunk < 4; ++chunk) {
        const uintptr_t chunk_ptr = mem.read<uintptr_t>(
            entity_list + sdk::list_chunk_offset + sizeof(uintptr_t) * (uintptr_t)chunk);
        if (!chunk_ptr || chunk_ptr < 0x10000) continue;

        for (int i = 0; i < 512; ++i) {
            const uintptr_t entity = mem.read<uintptr_t>(
                chunk_ptr + sdk::list_entry_stride * (uintptr_t)i);
            if (!entity || entity < 0x10000 || entity == local_pawn) continue;

            const int health = mem.read<int>(entity + offsets::m_iHealth);
            if (health <= 0 || health > 100) continue;

            const int team = mem.read<int>(entity + offsets::m_iTeamNum) & 0xFF;
            if (team != 2 && team != 3) continue;

            const Vec3 origin = mem.read<Vec3>(entity + offsets::m_vOldOrigin);
            if (origin.x == 0.0f && origin.y == 0.0f) continue;

            PlayerData p{};
            p.origin = origin;
            p.health = health;
            p.team   = team;

            const Vec3 d = origin - local_origin;
            p.distance = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) / 40.0f; // units -> ~metres

            // ── bones (optional add-on) ────────────────────────────────────
            // entity -> scene node -> modelState -> boneArray pointer.
            // If any hop fails, or the sanity check below fails, skeleton +
            // head-dot fallback off silently. Boxes/health/distance never care.
            const uintptr_t scene_node = mem.read<uintptr_t>(entity + offsets::m_pGameSceneNode);
            if (scene_node > 0x10000) {
                const uintptr_t bone_array = mem.read<uintptr_t>(
                    scene_node + offsets::m_modelState + offsets::m_boneArray);
                if (bone_array > 0x10000) {
                    BoneEntry buf[bones::count]{};
                    if (mem.read_bytes(bone_array, buf, sizeof(buf))) {
                        for (int b = 0; b < bones::count; ++b)
                            p.bone_pos[b] = buf[b].pos;

                        // Sanity: pelvis bone must sit near the pawn origin.
                        // If m_modelState ever changes in a game update, this
                        // disables bones instead of drawing garbage.
                        const Vec3 pel = p.bone_pos[bones::pelvis];
                        const float dx = pel.x - origin.x;
                        const float dy = pel.y - origin.y;
                        const float dz = pel.z - origin.z;
                        p.bones_ok = (dx*dx + dy*dy + dz*dz) < (120.0f * 120.0f);
                    }
                }
            }

            fresh.push_back(p);
        }
    }

    std::lock_guard<std::mutex> lock(world_mutex_);
    world_players_ = std::move(fresh);
}

// Render thread: fresh view matrix every frame -> no mouse-turn lag.
std::vector<PlayerESP> ESP::project(const Memory& mem, uintptr_t client_base,
                                    int screen_w, int screen_h) {
    std::vector<PlayerData> snapshot;
    {
        std::lock_guard<std::mutex> lock(world_mutex_);
        snapshot = world_players_;
    }

    std::vector<PlayerESP> result;
    if (snapshot.empty()) return result;

    const Matrix4x4 vm = mem.read<Matrix4x4>(client_base + offsets::dwViewMatrix);

    result.reserve(snapshot.size());
    for (const auto& p : snapshot) {
        Vec2 s_feet, s_head;

        if (!WorldToScreen(p.origin, s_feet, vm, screen_w, screen_h)) continue;

        Vec3 head_world = p.bones_ok
            ? p.bone_pos[bones::head]
            : Vec3(p.origin.x, p.origin.y, p.origin.z + kHeadFallbackHeight);

        if (!WorldToScreen(head_world, s_head, vm, screen_w, screen_h)) continue;

        const float box_h = s_feet.y - s_head.y;
        if (box_h < 5.0f) continue;

        PlayerESP e{};
        e.head     = s_head;
        e.feet     = s_feet;
        e.box_h    = box_h;
        e.box_w    = box_h * 0.45f;
        e.health   = p.health;
        e.team     = p.team;
        e.distance = p.distance;
        e.bones_ok = p.bones_ok;

        if (p.bones_ok) {
            for (int b = 0; b < bones::count; ++b)
                e.bone_ok[b] = WorldToScreen(p.bone_pos[b], e.bone_screen[b], vm, screen_w, screen_h);
        }

        result.push_back(e);
    }
    return result;
}

void ESP::draw_head_dot(ImDrawList* dl, const PlayerESP& p, ImU32 col) {
    const ImVec2 c(p.head.x, p.head.y);
    float r = p.box_h * 0.05f;
    if (r < 2.0f) r = 2.0f;
    dl->AddCircleFilled(c, r, col, 14);
    dl->AddCircle(c, r, IM_COL32(0, 0, 0, 200), 14, 1.0f);
}

void ESP::draw_skeleton(ImDrawList* dl, const PlayerESP& p, ImU32 col, float thickness) {
    if (!p.bones_ok) return;
    for (int i = 0; i < bones::kNumLinks; ++i) {
        const bones::Link& lk = bones::kLinks[i];
        if (!p.bone_ok[lk.a] || !p.bone_ok[lk.b]) continue;
        dl->AddLine(
            ImVec2(p.bone_screen[lk.a].x, p.bone_screen[lk.a].y),
            ImVec2(p.bone_screen[lk.b].x, p.bone_screen[lk.b].y),
            col, thickness);
    }
}
