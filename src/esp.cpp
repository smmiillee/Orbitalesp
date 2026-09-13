// --- src/esp.cpp ---
#include "esp.h"
#include <cmath>

bool ESP::world_to_screen(const Vec3& world, Vec2& screen,
                           const Matrix4x4& vm, int screen_w, int screen_h) {
    float w = vm.m[3][0] * world.x + vm.m[3][1] * world.y
            + vm.m[3][2] * world.z + vm.m[3][3];
    if (w < 0.001f) return false;

    float x = vm.m[0][0] * world.x + vm.m[0][1] * world.y
            + vm.m[0][2] * world.z + vm.m[0][3];
    float y = vm.m[1][0] * world.x + vm.m[1][1] * world.y
            + vm.m[1][2] * world.z + vm.m[1][3];

    float inv_w = 1.0f / w;
    screen.x = (screen_w / 2.0f) + (x * inv_w) * (screen_w / 2.0f);
    screen.y = (screen_h / 2.0f) - (y * inv_w) * (screen_h / 2.0f);
    return true;
}

static void calibrate(const Memory& mem, uintptr_t entity_list,
                      uintptr_t local_pawn,
                      uintptr_t& best_off, uintptr_t& best_stride) {
    struct Combo { uintptr_t off; uintptr_t stride; };
    constexpr Combo combos[] = {
        {0x10, 112}, {0x10, 120}, {0x08, 112}, {0x08, 120}
    };
    int best_score = -1;
    best_off = 0x10; best_stride = 120;
    for (const auto& c : combos) {
        int score = 0;
        for (int chunk = 0; chunk < 4; chunk++) {
            uintptr_t cp = mem.read<uintptr_t>(entity_list + c.off + 8 * chunk);
            if (!cp || cp < 0x10000) continue;
            for (int i = 0; i < 512; i++) {
                uintptr_t ent = mem.read<uintptr_t>(cp + c.stride * i);
                if (!ent || ent < 0x10000 || ent == local_pawn) continue;
                int hp = mem.read<int>(ent + offsets::m_iHealth);
                if (hp <= 0 || hp > 100) continue;
                int tm = mem.read<int>(ent + offsets::m_iTeamNum) & 0xFF;
                if (tm == 2 || tm == 3) score++;
            }
        }
        if (score > best_score) {
            best_score = score; best_off = c.off; best_stride = c.stride;
        }
    }
}

void ESP::update(const Memory& mem, uintptr_t client_base, int screen_w, int screen_h) {
    std::vector<PlayerESP> new_players;

    Matrix4x4 vm          = mem.read<Matrix4x4>(client_base + offsets::dwViewMatrix);
    uintptr_t entity_list = mem.read<uintptr_t>(client_base + offsets::dwEntityList);
    uintptr_t local_pawn  = mem.read<uintptr_t>(client_base + offsets::dwLocalPlayerPawn);
    if (!entity_list || !local_pawn) {
        std::lock_guard<std::mutex> lock(players_mutex);
        players.clear();
        return;
    }

    static uintptr_t s_off = 0, s_stride = 0;
    static bool s_done = false;
    if (!s_done) {
        calibrate(mem, entity_list, local_pawn, s_off, s_stride);
        s_done = true;
    }

    for (int chunk = 0; chunk < 4; chunk++) {
        uintptr_t chunk_ptr = mem.read<uintptr_t>(entity_list + s_off + 8 * chunk);
        if (!chunk_ptr || chunk_ptr < 0x10000) continue;

        for (int i = 0; i < 512; i++) {
            uintptr_t entity = mem.read<uintptr_t>(chunk_ptr + s_stride * i);
            if (!entity || entity < 0x10000 || entity == local_pawn) continue;

            int health = mem.read<int>(entity + offsets::m_iHealth);
            if (health <= 0 || health > 100) continue;

            int team = mem.read<int>(entity + offsets::m_iTeamNum) & 0xFF;
            if (team != 2 && team != 3) continue;

            Vec3 origin;
            origin.x = mem.read<float>(entity + offsets::m_vOldOrigin);
            origin.y = mem.read<float>(entity + offsets::m_vOldOrigin + 4);
            origin.z = mem.read<float>(entity + offsets::m_vOldOrigin + 8);
            if (origin.x == 0.0f && origin.y == 0.0f) continue;

            Vec3 head = { origin.x, origin.y, origin.z + 70.0f };
            Vec2 screen_head, screen_feet;
            if (!world_to_screen(head,   screen_head, vm, screen_w, screen_h)) continue;
            if (!world_to_screen(origin, screen_feet, vm, screen_w, screen_h)) continue;

            float box_h = screen_feet.y - screen_head.y;
            if (box_h < 5.0f) continue;

            float dist = std::sqrt(
                origin.x * origin.x +
                origin.y * origin.y +
                origin.z * origin.z) / 40.0f;

            new_players.push_back({
                origin, screen_head, screen_feet,
                health, team, true, dist,
                box_h, box_h * 0.45f
            });
        }
    }

    // Swap under lock — render thread never sees a partial write
    std::lock_guard<std::mutex> lock(players_mutex);
    players = std::move(new_players);
}
