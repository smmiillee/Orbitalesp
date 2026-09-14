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

void lerp3(Vec3& o, const Vec3& a, const Vec3& b, float f) {
    o.x = a.x + (b.x - a.x) * f;
    o.y = a.y + (b.y - a.y) * f;
    o.z = a.z + (b.z - a.z) * f;
}

Vec3 read_origin(const Memory& mem, uintptr_t e) {
    Vec3 v{};
    mem.read_bytes(e + offsets::m_vOldOrigin, &v, sizeof(v));
    return v;
}

struct EntHead { int health = 0; int team = 0; };

EntHead read_ent_head(const Memory& mem, uintptr_t e) {
    EntHead o;
    uint8_t buf[0x100];
    if (mem.read_bytes(e + 0x340, buf, sizeof(buf))) {
        std::memcpy(&o.health, buf + (offsets::m_iHealth - 0x340), 4);
        o.team = buf[offsets::m_iTeamNum - 0x340];
        return o;
    }
    o.health = mem.read<int>(e + offsets::m_iHealth);
    o.team   = mem.read<uint8_t>(e + offsets::m_iTeamNum);
    return o;
}

void enumerate_slots(const Memory& mem, uintptr_t el, uintptr_t chunk_off,
                     uintptr_t stride, int chunks, std::vector<uintptr_t>& out) {
    out.clear();
    if (!stride) return;

    constexpr size_t PER = 128;
    static std::vector<uint8_t> buf;
    if (buf.size() < PER * stride) buf.resize(PER * stride);

    for (int ch = 0; ch < chunks; ++ch) {
        const uintptr_t base = mem.read<uintptr_t>(el + chunk_off + 8 * ch);
        if (!valid_ptr(base)) continue;

        for (size_t s0 = 0; s0 < (size_t)offsets::kSlots; s0 += PER) {
            const size_t n = (std::min)(PER, (size_t)offsets::kSlots - s0);
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

uintptr_t resolve_handle(const Memory& mem, uintptr_t el, uintptr_t chunk_off,
                         uintptr_t stride, uint32_t h) {
    const uint32_t idx = h & 0x7FFF;
    if (!idx) return 0;
    const uintptr_t chunk =
        mem.read<uintptr_t>(el + chunk_off + 8 * (idx >> 9));
    if (!valid_ptr(chunk)) return 0;
    return mem.read<uintptr_t>(chunk + stride * (idx & 0x1FF));
}

void sanitize(char* dst, size_t cap, const char* src, size_t len) {
    size_t j = 0;
    for (size_t i = 0; i + 1 < cap && i < len; ++i) {
        const unsigned char c = (unsigned char)src[i];
        if (!c) break;
        if (c >= 32 && c < 127) dst[j++] = (char)c;
    }
    dst[j] = '\0';
}

struct WeaponName { int id; const char* name; };

constexpr WeaponName kWeapons[] = {
    { 1, "Deagle" },      { 2, "Dualies" },     { 3, "57" },
    { 4, "Glock" },       { 5, "Bowie" },       { 7, "AK-47" },
    { 8, "AUG" },         { 9, "AWP" },         { 10, "FAMAS" },
    { 11, "Auto Sniper" },{ 12, "Butterfly" },  { 13, "GALIL" },
    { 14, "M249" },       { 16, "M4A4" },       { 17, "MAC-10" },
    { 19, "P90" },        { 20, "Mag-7" },      { 23, "MP5-SD" },
    { 24, "UMP-45" },     { 25, "XM" },         { 26, "PP-Bizon" },
    { 27, "MAG-7" },      { 28, "Negev" },      { 29, "Sawed-Off" },
    { 30, "Tec 9" },      { 31, "Zeus" },       { 32, "P2K" },
    { 33, "MP7" },        { 34, "MP9" },        { 35, "Nova" },
    { 36, "P250" },       { 38, "Auto Sniper" },{ 39, "SG 553" },
    { 40, "Scout" },      { 43, "Flashbang" },  { 44, "HE Grenade" },
    { 45, "Smoke" },      { 46, "Molotov" },    { 47, "Decoy" },
    { 48, "Incendiary" }, { 49, "C4" },         { 57, "Health Shot" },
    { 60, "M4A1" },       { 61, "Usp" },        { 63, "CZ" },
    { 64, "R8" },         { 68, "Knife" },      { 80, "Knife" },
    { 81, "Knife" },      { 82, "Knife" },      { 83, "Knife" },
};

const char* weapon_name(int id) {
    for (const WeaponName& w : kWeapons)
        if (w.id == id) return w.name;
    if ((id >= 500 && id <= 600) || id == 42 || id == 59) return "Knife";
    return nullptr;
}

bool read_designer(const Memory& mem, uintptr_t ent, char* out, size_t cap) {
    out[0] = '\0';
    const uintptr_t l1 = mem.read<uintptr_t>(ent + offsets::m_designerLvl1);
    if (!valid_ptr(l1)) return false;
    const uintptr_t np = mem.read<uintptr_t>(l1 + offsets::m_designerPtr);
    if (!valid_ptr(np)) return false;

    char raw[48] = {};
    if (!mem.read_bytes(np, raw, 40)) return false;
    raw[39] = '\0';

    const char* s = raw;
    if (std::strncmp(s, "weapon_", 7) == 0) s += 7;
    sanitize(out, cap, s, std::strlen(s));
    return out[0] != '\0';
}

uint16_t item_def_index(const Memory& mem, uintptr_t w) {
    return mem.read<uint16_t>(w + offsets::m_AttributeManager +
                              offsets::m_Item +
                              offsets::m_iItemDefinitionIndex);
}

bool plausible_def(uint16_t id) {
    return weapon_name(id) != nullptr ||
           (id >= 1 && id <= 90) ||
           (id >= 500 && id <= 600);
}

struct WsvcProbe { uintptr_t off; bool ok; };

// Weapon services offset is discovered at runtime. Both weapons and the bomb
// carrier read through it, so a wrong hardcoded value made BOTH vanish.
WsvcProbe find_weapon_services(const Memory& mem, uintptr_t el,
                               uintptr_t chunk_off, uintptr_t stride,
                               uintptr_t local_pawn,
                               const std::vector<uintptr_t>& pawns) {
    WsvcProbe res{ 0, false };

    auto try_pawn = [&](uintptr_t p, uintptr_t off, bool need_name) -> bool {
        const uintptr_t ws = mem.read<uintptr_t>(p + off);
        if (!valid_ptr(ws)) return false;

        const uint32_t h = mem.read<uint32_t>(ws + offsets::m_hActiveWeapon);
        if (!h || h == 0xFFFFFFFFu) return false;

        const uintptr_t w = resolve_handle(mem, el, chunk_off, stride, h);
        if (!valid_ptr(w)) return false;
        if (!plausible_def(item_def_index(mem, w))) return false;

        if (need_name) {
            char nm[24] = {};
            if (!read_designer(mem, w, nm, sizeof(nm))) return false;
        }
        return true;
    };

    for (uintptr_t off = 0x1000; off <= 0x1500; off += 8) {
        if (!try_pawn(local_pawn, off, true)) continue;
        for (uintptr_t p : pawns) {
            if (p == local_pawn) continue;
            if (try_pawn(p, off, false)) {
                res.off = off;
                res.ok = true;
                return res;
            }
        }
    }
    return res;
}

struct BE { Vec3 pos; float pad[5]; };

int bone_points(const Memory& mem, uintptr_t arr, const Vec3& origin) {
    BE b[offsets::kBoneSlots];
    if (!mem.read_bytes(arr, b, sizeof(b))) return 0;

    int good = 0;
    for (int i = 0; i < offsets::kBoneSlots; ++i) {
        const Vec3& p = b[i].pos;
        if (!sane_vec(p)) continue;
        if (std::fabs(p.x - origin.x) > 110.0f) continue;
        if (std::fabs(p.y - origin.y) > 110.0f) continue;
        const float dz = p.z - origin.z;
        if (dz < -40.0f || dz > 150.0f) continue;
        ++good;
    }
    return good;
}

bool looks_like_entity(const Memory& mem, uintptr_t ent) {
    if (!valid_ptr(ent)) return false;
    return valid_ptr(
        mem.read<uintptr_t>(ent + offsets::m_pGameSceneNode));
}

} // namespace

bool ESP::world_to_screen(const Vec3& world, Vec2& screen,
                          const Matrix4x4& vm, int sw, int sh) {
    const float w = vm.m[3][0] * world.x + vm.m[3][1] * world.y
                  + vm.m[3][2] * world.z + vm.m[3][3];
    if (w < 0.001f) return false;

    const float x = vm.m[0][0] * world.x + vm.m[0][1] * world.y
                  + vm.m[0][2] * world.z + vm.m[0][3];
    const float y = vm.m[1][0] * world.x + vm.m[1][1] * world.y
                  + vm.m[1][2] * world.z + vm.m[1][3];

    const float inv = 1.0f / w;
    screen.x = (sw * 0.5f) + (x * inv) * (sw * 0.5f);
    screen.y = (sh * 0.5f) - (y * inv) * (sh * 0.5f);
    return true;
}

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

    static std::vector<uintptr_t> pawns;
    static double slots_at = -1e9;
    static double info_at  = -1e9;
    static bool   layout_locked = false;

    // ---- pawn list, about 10x per second ----
    if (now - slots_at > 100.0) {
        slots_at = now;

        struct L { uintptr_t off; uintptr_t stride; };
        static const L kL[] = {
            { 0x10, 120 }, { 0x10, 112 }, { 0x08, 120 }, { 0x08, 112 },
        };

        std::vector<uintptr_t> best;
        int bestn = -1;

        auto try_layout = [&](const L& l, int chunks) {
            std::vector<uintptr_t> all;
            std::vector<uintptr_t> got;
            enumerate_slots(mem, el, l.off, l.stride, chunks, all);

            for (uintptr_t e : all) {
                if (e == local_pawn) continue;
                const EntHead h = read_ent_head(mem, e);
                if (h.health <= 0 || h.health > 150) continue;
                if (h.team != 2 && h.team != 3) continue;
                got.push_back(e);
            }
            if ((int)got.size() > bestn) {
                bestn = (int)got.size();
                c_.chunk_off   = l.off;
                c_.slot_stride = l.stride;
                best = std::move(got);
            }
        };

        if (layout_locked) {
            try_layout({ c_.chunk_off, c_.slot_stride }, offsets::kChunks);
            if (bestn <= 0) layout_locked = false;
        }
        if (!layout_locked) {
            for (const L& l : kL) try_layout(l, offsets::kChunks);
            if (bestn > 0) layout_locked = true;
        }

        if (bestn > 0) pawns = std::move(best);
        else           pawns.clear();
    }

    // ---- weapon services, discovered once ----
    if (!c_.wsvc_ok && now - c_.wsvc_try > 1500.0) {
        c_.wsvc_try = now;

        std::vector<uintptr_t> probe = pawns;
        if (probe.empty()) probe.push_back(local_pawn);

        const WsvcProbe r = find_weapon_services(
            mem, el, c_.chunk_off, c_.slot_stride, local_pawn, probe);
        if (r.ok) {
            c_.wsvc    = r.off;
            c_.wsvc_ok = true;
        }
    }

    // ---- bone chain ----
    if (!c_.bones_ok || c_.bone_fail > 250) {
        if (c_.probe_cd > 0) {
            --c_.probe_cd;
        } else {
            struct Seed { uintptr_t n; uintptr_t a; };
            static const Seed kSeeds[] = {
                { 0x330, 0x240 }, { 0x338, 0x240 },
                { 0x330, 0x210 }, { 0x338, 0x210 },
                { 0x330, 0x1E0 }, { 0x338, 0x1E0 },
            };

            std::vector<std::pair<uintptr_t, Vec3>> test;
            if (sane_vec(local_origin))
                test.push_back({ local_pawn, local_origin });
            for (uintptr_t p : pawns) {
                if (test.size() >= 3) break;
                test.push_back({ p, read_origin(mem, p) });
            }

            for (const Seed& s : kSeeds) {
                int hits = 0;
                for (const auto& t : test) {
                    const uintptr_t n = mem.read<uintptr_t>(t.first + s.n);
                    if (!valid_ptr(n)) break;
                    const uintptr_t a = mem.read<uintptr_t>(n + s.a);
                    if (!valid_ptr(a)) break;
                    if (bone_points(mem, a, t.second) < 20) break;
                    ++hits;
                }
                const int need = (test.size() >= 2) ? 2 : 1;
                if (hits >= need) {
                    c_.bone_node = s.n;
                    c_.bone_arr  = s.a;
                    c_.bones_ok  = true;
                    c_.bone_fail = 0;
                    break;
                }
            }
            c_.probe_cd = 500;
        }
    }

    // ---- sample every live pawn ----
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

        if (c_.bones_ok) {
            const uintptr_t n = mem.read<uintptr_t>(p + c_.bone_node);
            const uintptr_t a = valid_ptr(n)
                ? mem.read<uintptr_t>(n + c_.bone_arr) : 0;
            if (valid_ptr(a)) {
                BE raw[BONE_COUNT];
                if (mem.read_bytes(a, raw, sizeof(raw))) {
                    int cnt = 0;
                    for (int i = 0; i < BONE_COUNT; ++i) {
                        const Vec3& q = raw[i].pos;
                        if (!sane_vec(q)) continue;
                        if (std::fabs(q.x - origin.x) > 60.0f) continue;
                        if (std::fabs(q.y - origin.y) > 60.0f) continue;
                        const float dz = q.z - origin.z;
                        if (dz < -25.0f || dz > 90.0f) continue;
                        s.bones[i]  = q;
                        s.bone_ok[i] = true;
                        ++cnt;
                    }
                    if (cnt >= 6) s.has_bones = true;
                }
            }
        }
        if (s.has_bones) ++bone_hits;

        s.head = (s.has_bones && s.bone_ok[BONE_HEAD])
                     ? s.bones[BONE_HEAD]
                     : Vec3{ origin.x, origin.y, origin.z + 64.0f };

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

        Track mv = *dst;
        mv.health   = h.health;
        mv.team     = h.team;
        mv.distance = std::sqrt(dx * dx + dy * dy + dz * dz) / 40.0f;
        mv.push(s);
        fresh.push_back(std::move(mv));
    }

    if (c_.bones_ok && !pawns.empty() && bone_hits == 0) ++c_.bone_fail;
    else                                                c_.bone_fail = 0;

    // ---- names, weapons, bomb carrier ----
    if (now - info_at > 300.0) {
        info_at = now;

        for (Track& t : fresh) {
            t.has_bomb = false;
            t.weapon[0] = '\0';

            const uint32_t hc =
                mem.read<uint32_t>(t.pawn + offsets::m_hController);
            if (hc && hc != 0xFFFFFFFFu) {
                const uintptr_t ctrl = resolve_handle(
                    mem, el, c_.chunk_off, c_.slot_stride, hc);
                if (valid_ptr(ctrl)) {
                    char raw[128] = {};
                    if (mem.read_bytes(ctrl + offsets::m_iszPlayerName,
                                       raw, sizeof(raw))) {
                        raw[127] = '\0';
                        sanitize(t.name, sizeof(t.name), raw, sizeof(raw));
                    }
                }
            }

            if (!c_.wsvc_ok) continue;

            const uintptr_t ws = mem.read<uintptr_t>(t.pawn + c_.wsvc);
            if (!valid_ptr(ws)) continue;

            const uint32_t hw =
                mem.read<uint32_t>(ws + offsets::m_hActiveWeapon);
            if (!hw || hw == 0xFFFFFFFFu) continue;

            const uintptr_t w = resolve_handle(
                mem, el, c_.chunk_off, c_.slot_stride, hw);
            if (!valid_ptr(w)) continue;

            const uint16_t id = item_def_index(mem, w);
            if (id == offsets::kItemDefC4) {
                t.has_bomb = true;
                std::snprintf(t.weapon, sizeof(t.weapon), "C4");
                continue;
            }

            const char* nm = weapon_name(id);
            if (nm) {
                std::snprintf(t.weapon, sizeof(t.weapon), "%s", nm);
            } else {
                char dn[24] = {};
                if (read_designer(mem, w, dn, sizeof(dn)))
                    std::snprintf(t.weapon, sizeof(t.weapon), "%s", dn);
            }
        }
    }

    // ---- planted C4 ----
    bool bomb_on = false;
    Vec3 bomb{};

    const uintptr_t pl =
        mem.read<uintptr_t>(client_base + offsets::dwPlantedC4);

    uintptr_t c4 = 0;
    if (looks_like_entity(mem, pl)) {
        c4 = pl;
    } else if (valid_ptr(pl)) {
        const uintptr_t inner = mem.read<uintptr_t>(pl);
        if (looks_like_entity(mem, inner)) c4 = inner;
    }

    if (c4) {
        const uintptr_t node =
            mem.read<uintptr_t>(c4 + offsets::m_pGameSceneNode);

        if (valid_ptr(node)) {
            static const uintptr_t kCand[] = {
                offsets::m_vecAbsOrigin, 0xD0, 0xC8,
            };

            auto accept = [&](const Vec3& v) {
                if (!sane_vec(v)) return false;
                const float dx = v.x - local_origin.x;
                const float dy = v.y - local_origin.y;
                const float dz = v.z - local_origin.z;
                return dx * dx + dy * dy + dz * dz < 12000.0f * 12000.0f;
            };

            auto read_pos = [&](uintptr_t off, Vec3& v) {
                v.x = mem.read<float>(node + off);
                v.y = mem.read<float>(node + off + 4);
                v.z = mem.read<float>(node + off + 8);
                return sane_vec(v);
            };

            if (c_.c4_ok) {
                Vec3 v{};
                if (read_pos(c_.c4_off, v) && accept(v)) {
                    bomb = v;
                    bomb_on = true;
                } else {
                    c_.c4_ok = false;
                }
            }

            if (!c_.c4_ok) {
                for (uintptr_t off : kCand) {
                    Vec3 v{};
                    if (!read_pos(off, v) || !accept(v)) continue;

                    if (c_.c4_off == off && c_.c4_last_t > 0.0 &&
                        std::fabs(v.z - c_.c4_last.z) < 0.5f) {
                        ++c_.c4_stable;
                    } else if (c_.c4_off != off) {
                        c_.c4_off = off;
                        c_.c4_stable = 1;
                    }
                    c_.c4_last = v;
                    c_.c4_last_t = now;

                    if (c_.c4_stable >= 3) {
                        c_.c4_ok = true;
                        bomb = v;
                        bomb_on = true;
                        break;
                    }
                }
            }
        }
    }

    std::lock_guard<std::mutex> lk(mtx);
    tracks_       = std::move(fresh);
    bomb_active_  = bomb_on;
    bomb_origin_  = bomb;
    players_alive = (int)tracks_.size();
}

namespace {

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
        const Snap a = at(i);
        const Snap b = at(i + 1);
        if (rt >= b.t && rt <= a.t) {
            const double span = a.t - b.t;
            const float f = (span > 0.0001)
                ? (float)((rt - b.t) / span) : 0.0f;
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

bool jumped(const Snap& a, const Snap& b) {
    const float dx = a.origin.x - b.origin.x;
    const float dy = a.origin.y - b.origin.y;
    const float dz = a.origin.z - b.origin.z;
    return dx * dx + dy * dy + dz * dz > 500.0f * 500.0f;
}

} // namespace

std::vector<PlayerESP> ESP::project(const Memory& mem, uintptr_t client_base,
                                    int sw, int sh, bool interpolate) {
    const Matrix4x4 vm =
        mem.read<Matrix4x4>(client_base + offsets::dwViewMatrix);

    const double rt = interpolate
        ? now_ms() - (double)interp_delay_ms
        : now_ms();

    std::lock_guard<std::mutex> lk(mtx);

    std::vector<PlayerESP> out;
    out.reserve(tracks_.size());

    for (const Track& t : tracks_) {
        Snap s{};
        if (!sample_hist(t, rt, s)) continue;

        const Snap newest = t.hist[(t.head - 1 + Track::kHist) % Track::kHist];
        if (jumped(s, newest)) s = newest;

        const Vec3 top{ s.head.x, s.head.y, s.head.z + 6.0f };

        PlayerESP e;
        if (!world_to_screen(s.head,   e.screen_head, vm, sw, sh)) continue;
        if (!world_to_screen(top,      e.screen_top,  vm, sw, sh)) continue;
        if (!world_to_screen(s.origin, e.screen_feet, vm, sw, sh)) continue;

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
                if (!world_to_screen(s.bones[i], sp, vm, sw, sh)) continue;
                e.bones[i]   = sp;
                e.bone_ok[i] = true;
            }
        }

        out.push_back(e);
    }

    return out;
}

BombESP ESP::project_bomb(const Memory& mem, uintptr_t client_base,
                          int sw, int sh) {
    BombESP b;
    std::lock_guard<std::mutex> lk(mtx);
    if (!bomb_active_) return b;

    const Matrix4x4 vm =
        mem.read<Matrix4x4>(client_base + offsets::dwViewMatrix);

    const uintptr_t lp =
        mem.read<uintptr_t>(client_base + offsets::dwLocalPlayerPawn);
    const Vec3 lo = read_origin(mem, lp);

    if (!world_to_screen(bomb_origin_, b.screen, vm, sw, sh)) return b;

    const Vec3 top{ bomb_origin_.x, bomb_origin_.y, bomb_origin_.z + 22.0f };
    if (!world_to_screen(top, b.screen_top, vm, sw, sh)) return b;

    const float dx = bomb_origin_.x - lo.x;
    const float dy = bomb_origin_.y - lo.y;
    const float dz = bomb_origin_.z - lo.z;
    b.distance = std::sqrt(dx * dx + dy * dy + dz * dz) / 40.0f;
    b.active   = true;
    return b;
}
