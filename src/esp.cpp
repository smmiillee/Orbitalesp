// --- src/esp.cpp ---
#include "esp.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace {

double now_ms() {
    static const auto t0 = std::chrono::steady_clock::now();
    return std::chrono::duration<double, std::milli>(
               std::chrono::steady_clock::now() - t0).count();
}

bool valid_ptr(uintptr_t p) {
    return p > 0x10000 && p < 0x00007FFFFFFF0000ULL && (p % 4) == 0;
}

bool sane_vec(const Vec3& v) {
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z) &&
           std::fabs(v.x) < 100000.0f && std::fabs(v.y) < 100000.0f &&
           std::fabs(v.z) < 100000.0f &&
           !(v.x == 0.0f && v.y == 0.0f && v.z == 0.0f);
}

void lerp3(Vec3& out, const Vec3& a, const Vec3& b, float f) {
    out.x = a.x + (b.x - a.x) * f;
    out.y = a.y + (b.y - a.y) * f;
    out.z = a.z + (b.z - a.z) * f;
}

Vec3 read_origin(const Memory& mem, uintptr_t ent) {
    Vec3 v{};
    mem.read_bytes(ent + offsets::m_vOldOrigin, &v, sizeof(v));
    return v;
}

// ── entity access ────────────────────────────────────────────────────────

struct EntHead { int health = 0; int team = 0; };

// One 0x100 window covers health and team. Falls back to two separate reads --
// the path the radar uses, so a refused bulk read can't empty the list.
EntHead read_ent_head(const Memory& mem, uintptr_t ent) {
    EntHead o;
    uint8_t buf[0x100];
    if (mem.read_bytes(ent + 0x340, buf, sizeof(buf))) {
        std::memcpy(&o.health, buf + (offsets::m_iHealth - 0x340), 4);
        o.team = buf[offsets::m_iTeamNum - 0x340];
        return o;
    }
    o.health = mem.read<int>(ent + offsets::m_iHealth);
    o.team   = mem.read<uint8_t>(ent + offsets::m_iTeamNum);
    return o;
}

// ── entity-list enumeration ──────────────────────────────────────────────
// Each read is a WHOLE NUMBER OF SLOTS. Reading fixed 16 KB pieces used to
// step off the slot grid (16384 / 120 = 136.5), so every piece after the first
// read from 64 bytes inside a slot and returned noise.
void enumerate_slots(const Memory& mem, uintptr_t el, uintptr_t chunk_off,
                     uintptr_t stride, std::vector<uintptr_t>& out) {
    out.clear();
    if (!stride) return;

    constexpr size_t kSlotsPerRead = 128;
    const size_t piece_bytes = kSlotsPerRead * stride;

    static std::vector<uint8_t> buf;
    if (buf.size() < piece_bytes) buf.resize(piece_bytes);

    for (int ch = 0; ch < offsets::kChunks; ++ch) {
        const uintptr_t base = mem.read<uintptr_t>(el + chunk_off + 8 * ch);
        if (!valid_ptr(base)) continue;

        for (size_t s0 = 0; s0 < static_cast<size_t>(offsets::kSlots);
             s0 += kSlotsPerRead) {
            const size_t n = (std::min)(kSlotsPerRead,
                             static_cast<size_t>(offsets::kSlots) - s0);
            if (!mem.read_bytes(base + s0 * stride, buf.data(), n * stride))
                break;
            for (size_t k = 0; k < n; ++k) {
                uintptr_t e = 0;
                std::memcpy(&e, buf.data() + k * stride, 8);
                if (valid_ptr(e)) out.push_back(e);
            }
        }
    }
}

// Slots -> live player pawns, skipping the local player and anyone dead.
void collect_pawns(const Memory& mem, uintptr_t el, uintptr_t chunk_off,
                   uintptr_t stride, uintptr_t local_pawn,
                   std::vector<uintptr_t>& all, std::vector<uintptr_t>& out) {
    enumerate_slots(mem, el, chunk_off, stride, all);

    out.clear();
    for (uintptr_t e : all) {
        if (e == local_pawn) continue;              // never box ourselves
        const EntHead h = read_ent_head(mem, e);
        if (h.health <= 0 || h.health > 150) continue;
        if (h.team != 2 && h.team != 3) continue;
        out.push_back(e);
    }
}

// A handle is an entity index: 9 bits of chunk, 9 bits of slot.
uintptr_t resolve_handle(const Memory& mem, uintptr_t el, uintptr_t chunk_off,
                         uintptr_t stride, uint32_t handle) {
    const uint32_t idx = handle & 0x7FFF;
    if (!idx) return 0;
    const uintptr_t chunk =
        mem.read<uintptr_t>(el + chunk_off + 8 * (idx >> 9));
    if (!valid_ptr(chunk)) return 0;
    return mem.read<uintptr_t>(chunk + stride * (idx & 0x1FF));
}

void sanitize(char* dst, size_t cap, const char* src, size_t src_len) {
    size_t j = 0;
    for (size_t i = 0; i + 1 < cap && i < src_len; ++i) {
        const unsigned char ch = static_cast<unsigned char>(src[i]);
        if (ch == 0) break;
        if (ch >= 32 && ch < 127) dst[j++] = static_cast<char>(ch);
    }
    dst[j] = '\0';
}

// ── weapon name (designer string) ────────────────────────────────────────
// The radar reads the weapon's designer name rather than mapping item indices,
// and that is what makes WEAPON ESP work. Chain:
//     weapon + 0x10 -> firstLevel
//     firstLevel + 0x20 -> char* ("weapon_ak47")
const char* prettify(const char* n) {
    struct Map { const char* in; const char* out; };
    static const Map kMap[] = {
        {"ak47","AK-47"}, {"m4a1","M4A4"}, {"m4a1_silencer","M4A1-S"},
        {"awp","AWP"}, {"deagle","Desert Eagle"}, {"glock","Glock-18"},
        {"usp_silencer","USP-S"}, {"hkp2000","P2000"}, {"p250","P250"},
        {"fiveseven","Five-SeveN"}, {"tec9","Tec-9"}, {"cz75a","CZ75-Auto"},
        {"revolver","R8 Revolver"}, {"elite","Dual Berettas"}, {"ssg08","SSG 08"},
        {"sg556","SG 553"}, {"aug","AUG"}, {"famas","FAMAS"},
        {"galilar","Galil AR"}, {"g3sg1","G3SG1"}, {"scar20","SCAR-20"},
        {"mp9","MP9"}, {"mp7","MP7"}, {"mp5sd","MP5-SD"}, {"ump45","UMP-45"},
        {"p90","P90"}, {"bizon","PP-Bizon"}, {"mac10","MAC-10"},
        {"xm1014","XM1014"}, {"mag7","MAG-7"}, {"sawedoff","Sawed-Off"},
        {"nova","Nova"}, {"negev","Negev"}, {"m249","M249"},
        {"taser","Zeus x27"}, {"smokegrenade","Smoke"}, {"flashbang","Flashbang"},
        {"hegrenade","HE Grenade"}, {"molotov","Molotov"},
        {"incgrenade","Incendiary"}, {"decoy","Decoy"}, {"c4","C4"},
        {"healthshot","Health Shot"},
    };
    for (const Map& m : kMap)
        if (std::strcmp(n, m.in) == 0) return m.out;
    if (std::strncmp(n, "knife", 5) == 0 || std::strncmp(n, "bayonet", 7) == 0)
        return "Knife";
    return nullptr;   // unknown -> show the raw name
}

struct WeaponInfo {
    char name[24]{};
    bool is_c4 = false;
    bool ok = false;
};

// pawn -> weapon services -> active weapon -> designer name (+ C4 check).
WeaponInfo read_weapon(const Memory& mem, uintptr_t el, uintptr_t chunk_off,
                       uintptr_t stride, uintptr_t pawn) {
    WeaponInfo out;

    const uintptr_t ws = mem.read<uintptr_t>(pawn + offsets::m_pWeaponServices);
    if (!valid_ptr(ws)) return out;

    const uint32_t h = mem.read<uint32_t>(ws + offsets::m_hActiveWeapon);
    if (!h || h == 0xFFFFFFFFu) return out;

    const uintptr_t w = resolve_handle(mem, el, chunk_off, stride, h);
    if (!valid_ptr(w)) return out;

    const uintptr_t lvl1 = mem.read<uintptr_t>(w + offsets::m_designerLvl1);
    if (valid_ptr(lvl1)) {
        const uintptr_t np = mem.read<uintptr_t>(lvl1 + offsets::m_designerPtr);
        if (valid_ptr(np)) {
            char raw[48] = {};
            if (mem.read_bytes(np, raw, 40)) {
                raw[39] = '\0';
                const char* s = raw;
                if (std::strncmp(s, "weapon_", 7) == 0) s += 7;
                sanitize(out.name, sizeof(out.name), s, std::strlen(s));
            }
        }
    }

    // Secondary C4 check through the embedded econ item, in case the designer
    // string is unavailable for this particular weapon entity.
    if (!out.is_c4) {
        const uint16_t id = mem.read<uint16_t>(
            w + offsets::m_AttributeManager + offsets::m_Item +
            offsets::m_iItemDefinitionIndex);
        if (id == offsets::kItemDefC4) out.is_c4 = true;
    }
    if (out.is_c4) std::snprintf(out.name, sizeof(out.name), "C4");

    out.ok = out.name[0] != '\0';
    return out;
}

void read_player_name(const Memory& mem, uintptr_t el, uintptr_t chunk_off,
                      uintptr_t stride, uintptr_t pawn, char* out, size_t cap) {
    out[0] = '\0';
    // pawn -> controller, which is the direction the schema defines.
    const uint32_t h = mem.read<uint32_t>(pawn + offsets::m_hController);
    if (!h || h == 0xFFFFFFFFu) return;

    const uintptr_t ctrl = resolve_handle(mem, el, chunk_off, stride, h);
    if (!valid_ptr(ctrl)) return;

    char raw[128] = {};
    if (!mem.read_bytes(ctrl + offsets::m_iszPlayerName, raw, sizeof(raw)))
        return;
    raw[127] = '\0';
    sanitize(out, cap, raw, sizeof(raw));
}

// ── skeleton ─────────────────────────────────────────────────────────────
constexpr float kHeadPad = 6.0f;
constexpr float kFallbackHeadZ = 64.0f;

struct BoneEntry { Vec3 pos; float pad[5]; };
static_assert(sizeof(BoneEntry) == offsets::m_boneStride,
              "CS2 bone stride must be 0x20");
struct TestPawn { uintptr_t pawn; Vec3 origin; };
struct BoneChain { uintptr_t node = 0, arr = 0; bool valid = false; };

int skeleton_points(const Memory& mem, uintptr_t arr, const Vec3& origin) {
    BoneEntry b[offsets::kBoneSlots];
    if (!mem.read_bytes(arr, b, sizeof(b))) return 0;

    int good = 0;
    for (int i = 0; i < offsets::kBoneSlots; ++i) {
        const Vec3& p = b[i].pos;
        if (!sane_vec(p)) continue;
        const float dx = p.x - origin.x, dy = p.y - origin.y, dz = p.z - origin.z;
        if (std::fabs(dx) > 110.0f || std::fabs(dy) > 110.0f) continue;
        if (dz < -40.0f || dz > 150.0f) continue;
        ++good;
    }
    return good;
}

BoneChain probe_bones(const Memory& mem, const std::vector<TestPawn>& test) {
    BoneChain best;
    if (test.empty()) return best;

    int best_score = 0;
    const int need = (test.size() >= 2) ? 2 : 1;

    std::vector<uint8_t> node_buf(offsets::kNodeScanBytes);

    for (uintptr_t no = 0x8; no + 8 <= offsets::kPawnScanBytes; no += 8) {
        const uintptr_t node = mem.read<uintptr_t>(test[0].pawn + no);
        if (!valid_ptr(node)) continue;
        if (!mem.read_bytes(node, node_buf.data(), node_buf.size())) continue;

        for (uintptr_t ao = 0x8; ao + 8 <= offsets::kNodeScanBytes; ao += 8) {
            uintptr_t arr = 0;
            std::memcpy(&arr, node_buf.data() + ao, 8);
            if (!valid_ptr(arr)) continue;
            if (skeleton_points(mem, arr, test[0].origin) < 20) continue;

            int score = 1;
            for (size_t t = 1; t < test.size(); ++t) {
                const uintptr_t n = mem.read<uintptr_t>(test[t].pawn + no);
                if (!valid_ptr(n)) break;
                const uintptr_t a = mem.read<uintptr_t>(n + ao);
                if (!valid_ptr(a)) break;
                if (skeleton_points(mem, a, test[t].origin) < 20) break;
                ++score;
            }

            if (score > best_score) {
                best_score = score;
                best.node = no;
                best.arr  = ao;
            }
        }
    }

    best.valid = best_score >= need;
    return best;
}

bool read_bones(const Memory& mem, uintptr_t pawn, const Vec3& origin,
                const BoneChain& chain,
                std::array<Vec3, BONE_COUNT>& out,
                std::array<bool, BONE_COUNT>& ok) {
    out.fill(Vec3{});
    ok.fill(false);
    if (!chain.valid || !pawn) return false;

    const uintptr_t node = mem.read<uintptr_t>(pawn + chain.node);
    if (!valid_ptr(node)) return false;
    const uintptr_t arr = mem.read<uintptr_t>(node + chain.arr);
    if (!valid_ptr(arr)) return false;

    BoneEntry raw[BONE_COUNT];
    if (!mem.read_bytes(arr, raw, sizeof(raw))) return false;

    int n = 0;
    for (int i = 0; i < BONE_COUNT; ++i) {
        const Vec3& p = raw[i].pos;
        if (!sane_vec(p)) continue;
        const float dx = p.x - origin.x, dy = p.y - origin.y, dz = p.z - origin.z;
        if (std::fabs(dx) > 60.0f || std::fabs(dy) > 60.0f) continue;
        if (dz < -25.0f || dz > 90.0f) continue;
        out[i] = p;
        ok[i]  = true;
        ++n;
    }
    return n >= 6;
}

// ── planted C4 ───────────────────────────────────────────────────────────
// dwPlantedC4 is a pointer chain, and which variant it is differs by build, so
// both are tried exactly like the radar does: the value may BE the entity, or
// it may point at one.
uintptr_t scene_node_of(const Memory& mem, uintptr_t ent) {
    static const uintptr_t kCand[] = { 0x338, 0x340, 0x348, 0x350, 0x358, 0x360 };
    for (uintptr_t off : kCand) {
        const uintptr_t n = mem.read<uintptr_t>(ent + off);
        if (valid_ptr(n)) return n;
    }
    return 0;
}

bool looks_like_entity(const Memory& mem, uintptr_t ent) {
    if (!valid_ptr(ent)) return false;
    return scene_node_of(mem, ent) != 0;
}

bool read_scene_pos(const Memory& mem, uintptr_t node, Vec3& out) {
    // X at +0xC4, then Y and Z follow contiguously.
    out.x = mem.read<float>(node + offsets::m_vecAbsOrigin);
    out.y = mem.read<float>(node + offsets::m_vecAbsOrigin + 4);
    out.z = mem.read<float>(node + offsets::m_vecAbsOrigin + 8);
    return sane_vec(out);
}

bool read_planted_c4(const Memory& mem, uintptr_t client_base, Vec3& out) {
    const uintptr_t p = mem.read<uintptr_t>(client_base + offsets::dwPlantedC4);
    if (!valid_ptr(p)) return false;

    uintptr_t ent = 0;
    if (looks_like_entity(mem, p)) {
        ent = p;                       // direct entity pointer
    } else {
        const uintptr_t inner = mem.read<uintptr_t>(p);
        if (looks_like_entity(mem, inner)) ent = inner;   // pointer-to-pointer
    }
    if (!ent) return false;

    const uintptr_t node = scene_node_of(mem, ent);
    if (!node) return false;
    return read_scene_pos(mem, node, out);
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

    const float inv = 1.0f / w;
    screen.x = (screen_w * 0.5f) + (x * inv) * (screen_w * 0.5f);
    screen.y = (screen_h * 0.5f) - (y * inv) * (screen_h * 0.5f);
    return true;
}

// ── reader thread ────────────────────────────────────────────────────────
void ESP::update_world(const Memory& mem, uintptr_t client_base) {
    const double now = now_ms();

    const uintptr_t el =
        mem.read<uintptr_t>(client_base + offsets::dwEntityList);
    const uintptr_t local_pawn =
        mem.read<uintptr_t>(client_base + offsets::dwLocalPlayerPawn);

    if (!el || !local_pawn) {
        std::lock_guard<std::mutex> lk(mtx);
        tracks_.clear();
        bomb_active_ = false;
        players_alive = 0;
        return;
    }

    const Vec3 local_origin = read_origin(mem, local_pawn);

    static std::vector<uintptr_t> slots;
    static std::vector<uintptr_t> pawns;
    static double slots_at = -1e9;
    static double info_at  = -1e9;

    // ── refresh the pawn list ~10x/sec, auto-detecting the list layout ────
    if (now - slots_at > 100.0) {
        slots_at = now;

        struct Layout { uintptr_t off, stride; };
        static const Layout kLayouts[] = {
            {0x10, 120}, {0x10, 112}, {0x08, 120}, {0x08, 112},
        };

        int best = -1;
        std::vector<uintptr_t> best_all, best_pawns;
        for (const Layout& L : kLayouts) {
            std::vector<uintptr_t> all, got;
            collect_pawns(mem, el, L.off, L.stride, local_pawn, all, got);
            if (static_cast<int>(got.size()) > best) {
                best = static_cast<int>(got.size());
                c_.chunk_off   = L.off;
                c_.slot_stride = L.stride;
                best_all  = std::move(all);
                best_pawns = std::move(got);
            }
        }
        if (best > 0) {
            slots = std::move(best_all);
            pawns = std::move(best_pawns);
        } else {
            pawns.clear();
        }
        diag_slots = static_cast<int>(slots.size());
    }

    // ── bone chain, resolved once and re-probed if it goes stale ─────────
    if (!c_.bones_ok || c_.bone_fail > 250) {
        if (c_.probe_cd > 0) {
            --c_.probe_cd;
        } else {
            std::vector<TestPawn> test;
            if (sane_vec(local_origin))
                test.push_back({ local_pawn, local_origin });
            for (uintptr_t p : pawns) {
                if (test.size() >= 4) break;
                test.push_back({ p, read_origin(mem, p) });
            }
            const BoneChain bc = probe_bones(mem, test);
            if (bc.valid) {
                c_.bone_node = bc.node;
                c_.bone_arr  = bc.arr;
                c_.bones_ok  = true;
                c_.bone_fail = 0;
            }
            c_.probe_cd = 125;   // ~1 s at an 8 ms tick
        }
    }

    // ── per-tick sample of every live pawn ───────────────────────────────
    std::vector<Track> fresh;
    fresh.reserve(pawns.size());
    int bone_hits = 0;

    for (uintptr_t p : pawns) {
        const EntHead h = read_ent_head(mem, p);
        if (h.health <= 0 || h.health > 150) continue;

        const Vec3 origin = read_origin(mem, p);
        if (!sane_vec(origin)) continue;

        Snap s;
        s.t = now;
        s.origin = origin;
        s.has_bones = read_bones(mem, p, origin,
                                 BoneChain{ c_.bone_node, c_.bone_arr,
                                            c_.bones_ok },
                                 s.bones, s.bone_ok);
        if (s.has_bones) ++bone_hits;
        s.head = (s.has_bones && s.bone_ok[BONE_HEAD])
                     ? s.bones[BONE_HEAD]
                     : Vec3{ origin.x, origin.y, origin.z + kFallbackHeadZ };

        // Reuse the existing track so its interpolation history, name and
        // weapon survive between ticks.
        Track* dst = nullptr;
        for (Track& t : tracks_) {
            if (t.pawn == p) { dst = &t; break; }
        }
        if (!dst) {
            tracks_.push_back(Track{});
            dst = &tracks_.back();
            dst->pawn = p;
        }

        const float dx = origin.x - local_origin.x;
        const float dy = origin.y - local_origin.y;
        const float dz = origin.z - local_origin.z;

        Track moved = *dst;
        moved.health    = h.health;
        moved.team      = h.team;
        moved.distance  = std::sqrt(dx * dx + dy * dy + dz * dz) / 40.0f;
        moved.bone_health = s.has_bones ? 1 : 0;
        moved.push(s);
        fresh.push_back(std::move(moved));
    }

    if (c_.bones_ok && !pawns.empty() && bone_hits == 0) ++c_.bone_fail;
    else                                                c_.bone_fail = 0;

    // ── names + weapons: slow-moving, ~3x/sec ─────────────────────────────
    if (now - info_at > 300.0) {
        info_at = now;
        for (Track& t : fresh) {
            read_player_name(mem, el, c_.chunk_off, c_.slot_stride,
                             t.pawn, t.name, sizeof(t.name));

            const WeaponInfo w =
                read_weapon(mem, el, c_.chunk_off, c_.slot_stride, t.pawn);
            t.has_bomb = w.is_c4;
            if (w.ok) {
                const char* pretty = prettify(w.name);
                std::snprintf(t.weapon, sizeof(t.weapon), "%s",
                              pretty ? pretty : w.name);
            }
        }
    }

    // ── planted C4 ───────────────────────────────────────────────────────
    Vec3 bomb{};
    const bool bomb_active = read_planted_c4(mem, client_base, bomb);

    std::lock_guard<std::mutex> lk(mtx);
    tracks_       = std::move(fresh);
    bomb_active_  = bomb_active;
    bomb_origin_  = bomb;
    players_alive = static_cast<int>(tracks_.size());
}

// ── render thread ────────────────────────────────────────────────────────
namespace {

// Pick the two real frames bracketing `rt` and interpolate between them.
bool sample_hist(const Track& t, double rt, Snap& out) {
    if (t.count == 0) return false;

    auto at = [&](int back) {
        const int i = ((t.head - 1 - back) % Track::kHist + Track::kHist)
                      % Track::kHist;
        return t.hist[i];
    };

    const Snap newest = at(0);
    if (t.count == 1 || rt >= newest.t) { out = newest; return true; }

    const Snap oldest = at(t.count - 1);
    if (rt <= oldest.t) { out = oldest; return true; }

    for (int i = 0; i + 1 < t.count; ++i) {
        const Snap a = at(i);       // newer
        const Snap b = at(i + 1);   // older
        if (rt >= b.t && rt <= a.t) {
            const double span = a.t - b.t;
            const float f = span > 0.0001
                          ? static_cast<float>((rt - b.t) / span) : 0.0f;
            out = b;
            out.t = rt;
            lerp3(out.origin, b.origin, a.origin, f);
            lerp3(out.head,   b.head,   a.head,   f);
            out.has_bones = false;
            for (int k = 0; k < BONE_COUNT; ++k) {
                out.bone_ok[k] = b.bone_ok[k] && a.bone_ok[k];
                if (out.bone_ok[k]) {
                    lerp3(out.bones[k], b.bones[k], a.bones[k], f);
                    out.has_bones = true;
                }
            }
            return true;
        }
    }
    out = newest;
    return true;
}

// A teleport/respawn must snap, never smear across the map.
bool jumped(const Snap& a, const Snap& b) {
    const float dx = a.origin.x - b.origin.x;
    const float dy = a.origin.y - b.origin.y;
    const float dz = a.origin.z - b.origin.z;
    return dx * dx + dy * dy + dz * dz > 500.0f * 500.0f;
}

} // namespace

std::vector<PlayerESP> ESP::project(const Memory& mem, uintptr_t client_base,
                                    int screen_w, int screen_h) {
    const Matrix4x4 vm = mem.read<Matrix4x4>(
        client_base + offsets::dwViewMatrix);

    const double rt = now_ms() - static_cast<double>(interp_delay_ms);

    std::lock_guard<std::mutex> lk(mtx);

    std::vector<PlayerESP> out;
    out.reserve(tracks_.size());

    for (const Track& t : tracks_) {
        Snap s{};
        if (!sample_hist(t, rt, s)) continue;

        const Snap newest = t.hist[(t.head - 1 + Track::kHist) % Track::kHist];
        if (jumped(s, newest)) s = newest;

        const Vec3 box_top{ s.head.x, s.head.y, s.head.z + kHeadPad };

        PlayerESP e;
        if (!world_to_screen(s.head,   e.screen_head, vm, screen_w, screen_h)) continue;
        if (!world_to_screen(box_top,  e.screen_top,  vm, screen_w, screen_h)) continue;
        if (!world_to_screen(s.origin, e.screen_feet, vm, screen_w, screen_h)) continue;

        e.box_h = e.screen_feet.y - e.screen_top.y;
        if (e.box_h < 5.0f) continue;

        e.box_w     = e.box_h * 0.45f;
        e.health    = t.health;
        e.team      = t.team;
        e.distance  = t.distance;
        e.has_bones = s.has_bones;
        e.has_bomb  = t.has_bomb;
        std::snprintf(e.name,   sizeof(e.name),   "%s", t.name);
        std::snprintf(e.weapon, sizeof(e.weapon), "%s", t.weapon);

        if (s.has_bones) {
            for (int i = 0; i < BONE_COUNT; ++i) {
                if (!s.bone_ok[i]) continue;
                Vec2 sp{};
                if (!world_to_screen(s.bones[i], sp, vm, screen_w, screen_h))
                    continue;
                e.bones[i]   = sp;
                e.bone_ok[i] = true;
            }
        }

        out.push_back(e);
    }

    return out;
}

BombESP ESP::project_bomb(const Memory& mem, uintptr_t client_base,
                          int screen_w, int screen_h) {
    BombESP b;
    std::lock_guard<std::mutex> lk(mtx);
    if (!bomb_active_) return b;

    const Matrix4x4 vm = mem.read<Matrix4x4>(
        client_base + offsets::dwViewMatrix);

    const uintptr_t local_pawn =
        mem.read<uintptr_t>(client_base + offsets::dwLocalPlayerPawn);
    const Vec3 lo = read_origin(mem, local_pawn);

    if (!world_to_screen(bomb_origin_, b.screen, vm, screen_w, screen_h))
        return b;

    const Vec3 top{ bomb_origin_.x, bomb_origin_.y, bomb_origin_.z + 22.0f };
    if (!world_to_screen(top, b.screen_top, vm, screen_w, screen_h))
        return b;

    const float dx = bomb_origin_.x - lo.x;
    const float dy = bomb_origin_.y - lo.y;
    const float dz = bomb_origin_.z - lo.z;
    b.distance = std::sqrt(dx * dx + dy * dy + dz * dz) / 40.0f;
    b.active   = true;
    return b;
}
