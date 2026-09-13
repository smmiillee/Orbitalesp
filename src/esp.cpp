// --- src/esp.cpp ---
#include "esp.h"
#include "offsets.h"
#include "sdk.h"

#include <imgui.h>
#include <cmath>

// Head height fallback (used for the dot / box top when bone data is
// unavailable). Same value your old code used.
static constexpr float kHeadFallbackHeight = 70.0f;

// One entire bone table (ids 0..27) read in a single ReadProcessMemory call:
// 28 * 32 bytes.
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
    fresh.reserve(offsets::max_entities);

    // Chunk 0 of the entity list holds the 64 player controllers.
    const uintptr_t ctrl_chunk =
        mem.read<uintptr_t>(entity_list + sdk::list_chunk_offset);

    if (ctrl_chunk && ctrl_chunk > 0x10000) {
        for (int i = 0; i < offsets::max_entities; ++i) {
            const uintptr_t controller =
                mem.read<uintptr_t>(ctrl_chunk + sdk::list_entry_stride * (uintptr_t)i);
            if (!controller || controller < 0x10000) continue;

            // Controller -> pawn handle -> pawn pointer (standard resolution).
            const uint32_t pawn_handle =
                mem.read<uint32_t>(controller + offsets::m_hPlayerPawn);
            if (pawn_handle == 0xFFFFFFFFu) continue;

            const uint32_t  idx  = pawn_handle & 0x7FFF;
            const uintptr_t chunk = mem.read<uintptr_t>(
                entity_list + sdk::list_chunk_offset + 8u * (idx >> 9));
            if (!chunk || chunk < 0x10000) continue;

            const uintptr_t pawn =
                mem.read<uintptr_t>(chunk + sdk::list_entry_stride * (idx & 0x1FF));
            if (!pawn || pawn < 0x10000 || pawn == local_pawn) continue;

            const int health = mem.read<int>(pawn + offsets::m_iHealth);
            if (health <= 0 || health > 100) continue;

            const int team = mem.read<int>(pawn + offsets::m_iTeamNum) & 0xFF;
            if (team != 2 && team != 3) continue;

            const Vec3 origin = mem.read<Vec3>(pawn + offsets::m_vOldOrigin);
            if (origin.x == 0.0f && origin.y == 0.0f) continue;

            PlayerData p{};
            p.origin = origin;
            p.health = health;
            p.team   = team;

            const Vec3 d = origin - local_origin;
            p.distance   = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z) / 40.0f;

            // ── bones: pawn -> scene node -> model state -> bone array ──
            const uintptr_t scene_node = mem.read<uintptr_t>(pawn + offsets::m_pGameSceneNode);
            if (scene_node > 0x10000) {
                const uintptr_t bone_array = mem.read<uintptr_t>(
                    scene_node + offsets::m_modelState + offsets::m_boneArray);
                if (bone_array > 0x10000) {
                    BoneEntry buf[bones::count]{};
                    if (mem.read_bytes(bone_array, buf, sizeof(buf))) {
                        for (int b = 0; b < bones::count; ++b)
                            p.bone_pos[b] = buf[b].pos;

                        // Sanity: the pelvis bone must sit near the pawn origin.
                        // If your m_modelState value ever changes, this quietly
                        // disables skeleton/dot-bones instead of drawing garbage.
                        const Vec3 pel = p.bone_pos[bones::pelvis];
                        const float dx = pel.x - origin.x;
                        const float dy = pel.y - origin.y;
                        const float dz = pel.z - origin.z;
                        p.bones_ok = (dx * dx + dy * dy + dz * dz) < (120.0f * 120.0f);
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
        Vec2 s_head, s_feet;

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
