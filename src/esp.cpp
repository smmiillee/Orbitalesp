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
    { 1, "Deagle" },      { 2, "Dualies" },      { 3, "57" },
    { 4, "Glock" },       { 5, "Bowie" },        { 7, "AK-47" },
    { 8, "AUG" },         { 9, "AWP" },          { 10, "FAMAS" },
    { 11, "Auto Sniper" },{ 12, "Butterfly" },   { 13, "GALIL" },
    { 14, "M249" },       { 16, "M4A4" },        { 17, "MAC-10" },
    { 19, "P90" },        { 20, "Mag-7" },       { 23, "MP5-SD" },
    { 24, "UMP-45" },     { 25, "XM" },          { 26, "PP-Bizon" },
    { 27, "MAG-7" },      { 28, "Negev" },       { 29, "Sawed-Off" },
    { 30, "Tec 9" },      { 31, "Zeus" },        { 32, "P2K" },
    { 33, "MP7" },        { 34, "MP9" },         { 35, "Nova" },
    { 36, "P250" },       { 38, "Auto Sniper" }, { 39, "SG 553" },
    { 40, "Scout" },      { 43, "Flashbang" },   { 44, "HE Grenade" },
    { 45, "Smoke" },      { 46, "Molotov" },     { 47, "Decoy" },
    { 48, "Incendiary" }, { 49, "C4" },          { 57, "Health Shot" },
    { 60, "M4A1" },       { 61, "Usp" },         { 63, "CZ" },
    { 64, "R8" },         { 68, "Knife" },       { 80, "Knife" },
    { 81, "Knife" },      { 82, "Knife" },       { 83, "Knife" },
};

const char* weapon_name(int id) {
    for (const WeaponName& w : kWeapons)
        if (w.id == id) return w.name;
    if ((id > 500 && id <= 600) || id == 42 || id == 59) return "Knife";
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

// ---------------------------------------------------------------------------
// DEF-INDEX SELF-CALIBRATION
//
// Some weapons render as "#0" and others as lowercase designer names ("ak47",
// "hegrenade"). The lowercase ones prove the DESIGNER chain works and that the
// def-index read returns 0. A wrong def index also breaks the bomb carrier and
// the C4 entity scan, since both match on id 49.
//
// So rather than guess another offset, we find it using the designer name as a
// reference: if the designer says "ak47", the correct offset must read 7.
// ---------------------------------------------------------------------------

struct DMap { const char* designer; int id; const char* pretty; };

constexpr DMap kDesigner[] = {
    { "ak47",          7,  "AK-47" },
    { "m4a1",          16, "M4A4" },
    { "m4a1_silencer", 60, "M4A1" },
    { "awp",           9,  "AWP" },
    { "deagle",        1,  "Deagle" },
    { "glock",         4,  "Glock" },
    { "usp_silencer",  61, "Usp" },
    { "hkp2000",       32, "P2K" },
    { "p250",          36, "P250" },
    { "fiveseven",     3,  "57" },
    { "tec9",          30, "Tec 9" },
    { "cz75a",         63, "CZ" },
    { "revolver",      64, "R8" },
    { "elite",         2,  "Dualies" },
    { "ssg08",         40, "Scout" },
    { "sg556",         39, "SG 553" },
    { "aug",           8,  "AUG" },
    { "famas",         10, "FAMAS" },
    { "galilar",       13, "GALIL" },
    { "g3sg1",         11, "Auto Sniper" },
    { "scar20",        38, "Auto Sniper" },
    { "mp9",           34, "MP9" },
    { "mp7",           33, "MP7" },
    { "mp5sd",         23, "MP5-SD" },
    { "ump45",         24, "UMP-45" },
    { "p90",           19, "P90" },
    { "bizon",         26, "PP-Bizon" },
    { "mac10",         17, "MAC-10" },
    { "xm1014",        25, "XM" },
    { "mag7",          27, "MAG-7" },
    { "sawedoff",      29, "Sawed-Off" },
    { "nova",          35, "Nova" },
    { "negev",         28, "Negev" },
    { "m249",          14, "M249" },
    { "taser",         31, "Zeus" },
    { "smokegrenade",  45, "Smoke" },
    { "flashbang",     43, "Flashbang" },
    { "hegrenade",     44, "HE Grenade" },
    { "molotov",       46, "Molotov" },
    { "incgrenade",    48, "Incendiary" },
    { "decoy",         47, "Decoy" },
    { "c4",            49, "C4" },
};

int designer_id(const char* dn) {
    for (const DMap& d : kDesigner)
        if (std::strcmp(dn, d.designer) == 0) return d.id;
    if (std::strncmp(dn, "knife", 5) == 0) return 500;
    if (std::strncmp(dn, "bayonet", 7) == 0) return 500;
    return 0;
}

const char* pretty_from_designer(const char* dn) {
    for (const DMap& d : kDesigner)
        if (std::strcmp(dn, d.designer) == 0) return d.pretty;
    if (std::strncmp(dn, "knife", 5) == 0) return "Knife";
    if (std::strncmp(dn, "bayonet", 7) == 0) return "Knife";
    return nullptr;
}

bool plausible_id_val(int id) {
    return weapon_name(id) != nullptr ||
           (id >= 1 && id <= 90) ||
           (id >= 500 && id <= 600);
}

uintptr_t g_defidx_off = 0;

uint16_t item_def_index(const Memory& mem, uintptr_t w) {
    if (g_defidx_off) return mem.read<uint16_t>(w + g_defidx_off);

    const uint16_t direct =
        mem.read<uint16_t>(w + offsets::m_iItemDefinitionIndex);
    if (direct >= 1 && direct <= 700) return direct;

    const uint16_t via_item = mem.read<uint16_t>(
        w + offsets::m_AttributeManager + offsets::m_Item +
        offsets::m_iItemDefinitionIndex);
    if (via_item >= 1 && via_item <= 700) return via_item;

    return direct;
}

// Collect a weapon sample for every DISTINCT designer name we can find, so the
// calibration sees more than one weapon class.
struct DefSample { uintptr_t w; int expect; };

int collect_samples(const Memory& mem, uintptr_t el, uintptr_t chunk_off,
                    uintptr_t stride, const std::vector<uintptr_t>& pawns,
                    DefSample* out, int max_out) {
    int n = 0;
    char seen[16][24] = {};
    int  seen_n = 0;

    for (uintptr_t p : pawns) {
        if (n >= max_out) break;

        const uintptr_t ws = mem.read<uintptr_t>(p + offsets::m_pWeaponServices);
        if (!valid_ptr(ws)) continue;

        const uint32_t h = mem.read<uint32_t>(ws + offsets::m_hActiveWeapon);
        if (!h || h == 0xFFFFFFFFu) continue;

        const uintptr_t w = resolve_handle(mem, el, chunk_off, stride, h);
        if (!valid_ptr(w)) continue;

        char dn[24] = {};
        if (!read_designer(mem, w, dn, sizeof(dn))) continue;

        bool dup = false;
        for (int i = 0; i < seen_n; ++i)
            if (std::strcmp(seen[i], dn) == 0) { dup = true; break; }
        if (dup) continue;
        if (seen_n < 16) std::snprintf(seen[seen_n++], 24, "%s", dn);

        out[n].w = w;
        out[n].expect = designer_id(dn);
        ++n;
    }
    return n;
}

// Calibrate a LIST of offsets, not one. Any offset that agrees with a designer
// reference anywhere in the sample set is kept, so pistol offsets and rifle
// offsets can both be learned.
void calibrate_defidx(const Memory& mem, uintptr_t el, uintptr_t chunk_off,
                      uintptr_t stride,
                      const std::vector<uintptr_t>& pawns) {
    DefSample samples[24];
    const int n = collect_samples(mem, el, chunk_off, stride, pawns,
                                  samples, 24);
    if (n == 0) return;

    struct Best { uintptr_t off; int matches; int score; };
    Best cands[64];
    int cand_n = 0;

    for (uintptr_t off = 0x1000; off <= 0x1800; off += 2) {
        int score = 0, matches = 0;

        for (int i = 0; i < n; ++i) {
            const int id = mem.read<uint16_t>(samples[i].w + off);
            if (!plausible_id_val(id)) continue;
            ++score;
            if (samples[i].expect && id == samples[i].expect) {
                ++matches;
                score += 2;
            }
        }
        if (matches >= 1 && cand_n < 64)
            cands[cand_n++] = Best{ off, matches, score };
    }

    for (int i = 0; i < cand_n && g_def_off_n < kMaxDefOff; ++i) {
        int best = i;
        for (int j = i + 1; j < cand_n; ++j) {
            if (cands[j].matches > cands[best].matches ||
                (cands[j].matches == cands[best].matches &&
                 cands[j].score > cands[best].score))
                best = j;
        }
        const Best tmp = cands[i];
        cands[i] = cands[best];
        cands[best] = tmp;

        bool dup = false;
        for (int k = 0; k < g_def_off_n; ++k) {
            const int a = mem.read<uint16_t>(samples[0].w + g_def_offs[k]);
            const int b = mem.read<uint16_t>(samples[0].w + cands[i].off);
            if (a == b && a != 0) { dup = true; break; }
        }
        if (dup) continue;

        g_def_offs[g_def_off_n++] = cands[i].off;
    }
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

bool chain_hits(const Memory& mem, uintptr_t pawn, const Vec3& origin,
                uintptr_t node_off, uintptr_t arr_off) {
    const uintptr_t n = mem.read<uintptr_t>(pawn + node_off);
    if (!valid_ptr(n)) return false;
    const uintptr_t a = mem.read<uintptr_t>(n + arr_off);
    if (!valid_ptr(a)) return false;
    return bone_points(mem, a, origin) >= 20;
}

bool looks_like_entity(const Memory& mem, uintptr_t ent) {
    if (!valid_ptr(ent)) return false;
    return valid_ptr(mem.read<uintptr_t>(ent + offsets::m_pGameSceneNode));
}

bool wsvc_valid(const Memory& mem, uintptr_t el, uintptr_t chunk_off,
                uintptr_t stride, uintptr_t pawn, uintptr_t off) {
    const uintptr_t ws = mem.read<uintptr_t>(pawn + off);
    if (!valid_ptr(ws)) return false;

    const uint32_t h = mem.read<uint32_t>(ws + offsets::m_hActiveWeapon);
    if (!h || h == 0xFFFFFFFFu) return false;

    const uintptr_t w = resolve_handle(mem, el, chunk_off, stride, h);
    if (!valid_ptr(w)) return false;

    return looks_like_entity(mem, w);
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

    // ---- pawn list ----
    if (now - slots_at > 100.0) {
        slots_at = now;

        struct L { uintptr_t off; uintptr_t stride; };
        static const L kL[] = {
            { 0x10, 120 }, { 0x10, 112 }, { 0x08, 120 }, { 0x08, 112 },
        };

        std::vector<uintptr_t> best;
        int bestn = -1;

        auto try_layout = [&](const L& l, int chunks) {
            std::vector<uintptr_t> all, got;
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

    // ---- weapon services ----
    if (!c_.wsvc_ok && now - c_.wsvc_try > 1000.0) {
        c_.wsvc_try = now;

        std::vector<uintptr_t> probe = pawns;
        if (probe.empty()) probe.push_back(local_pawn);

        static const uintptr_t kCand[] = { 0x1208, 0x11E0, 0x11D8, 0x11E8,
                                           0x1200, 0x1210 };
        bool found = false;

        for (uintptr_t off : kCand) {
            if (!wsvc_valid(mem, el, c_.chunk_off, c_.slot_stride,
                            local_pawn, off))
                continue;
            int extra = 0;
            for (uintptr_t p : probe) {
                if (p == local_pawn) continue;
                if (wsvc_valid(mem, el, c_.chunk_off, c_.slot_stride, p, off))
                    ++extra;
            }
            if (extra >= 1) {
                c_.wsvc = off; c_.wsvc_ok = true; found = true;
                break;
            }
        }

        if (!found) {
            for (uintptr_t off = 0x1000; off <= 0x1600; off += 8) {
                if (!wsvc_valid(mem, el, c_.chunk_off, c_.slot_stride,
                                local_pawn, off))
                    continue;
                int extra = 0;
                for (uintptr_t p : probe) {
                    if (p == local_pawn) continue;
                    if (wsvc_valid(mem, el, c_.chunk_off, c_.slot_stride, p, off))
                        ++extra;
                }
                if (extra >= 1) { c_.wsvc = off; c_.wsvc_ok = true; break; }
            }
        }
    }

    diag_wsvc_ok = c_.wsvc_ok;
    diag_wsvc = c_.wsvc;

    // ---- def-index offset, calibrated from designer names ----
    if (!g_defidx_off && c_.wsvc_ok && !pawns.empty() &&
        now - c_.defidx_try > 2000.0) {
        c_.defidx_try = now;
        g_defidx_off = calibrate_defidx(mem, el, c_.chunk_off,
                                        c_.slot_stride, pawns);
        diag_defidx = (int)g_defidx_off;
    }

    // ---- bone chain: seeds, then bounded scan ----
    if (!c_.bones_ok || c_.bone_fail > 250) {
        if (c_.probe_cd > 0) {
            --c_.probe_cd;
        } else {
            std::vector<std::pair<uintptr_t, Vec3>> test;
            if (sane_vec(local_origin))
                test.push_back({ local_pawn, local_origin });
            for (uintptr_t p : pawns) {
                if (test.size() >= 4) break;
                test.push_back({ p, read_origin(mem, p) });
            }
            const int need = (test.size() >= 2) ? 2 : 1;

            bool found = false;

            struct Seed { uintptr_t n; uintptr_t a; };
            static const Seed kSeeds[] = {
                { 0x330, 0x240 }, { 0x338, 0x240 },
                { 0x330, 0x210 }, { 0x338, 0x210 },
                { 0x330, 0x1E0 }, { 0x338, 0x1E0 },
            };

            for (const Seed& s : kSeeds) {
                int hits = 0;
                for (const auto& t : test)
                    if (chain_hits(mem, t.first, t.second, s.n, s.a)) ++hits;
                if (hits >= need) {
                    c_.bone_node = s.n; c_.bone_arr = s.a;
                    found = true; break;
                }
            }

            if (!found && !test.empty()) {
                int best_hits = 0;
                uintptr_t bn = 0, ba = 0;
                for (uintptr_t no = 0x2C0; no <= 0x3A0; no += 8) {
                    for (uintptr_t ao = 0x180; ao <= 0x300; ao += 8) {
                        if (!chain_hits(mem, test[0].first, test[0].second,
                                        no, ao))
                            continue;
                        int hits = 1;
                        for (size_t i = 1; i < test.size(); ++i)
                            if (chain_hits(mem, test[i].first, test[i].second,
                                           no, ao))
                                ++hits;
                        if (hits > best_hits) {
                            best_hits = hits; bn = no; ba = ao;
                        }
                    }
                }
                if (best_hits >= need) {
                    c_.bone_node = bn; c_.bone_arr = ba; found = true;
                }
            }

            if (found) { c_.bones_ok = true; c_.bone_fail = 0; }
            c_.probe_cd = 375;
        }
    }

    diag_bones_ok  = c_.bones_ok;
    diag_bone_node = c_.bone_node;
    diag_bone_arr  = c_.bone_arr;

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
                        s.bones[i]   = q;
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
        for (Track& t : tracks_)
            if (t.pawn == p) { dst = &t; break; }
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

    // ---- names, weapons, carrier ----
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

            // *** CARRIED C4 IS NOT THE ACTIVE WEAPON ***
            // The carrier was only detected once they HELD the bomb, because we
            // only looked at m_hActiveWeapon. A C4 on someone's back is one of
            // their carried weapons, not the active one -- hence "only shows
            // when in hands".
            //
            // So we scan the entity list for the C4 entity and read its OWNER
            // instead. That is state-independent: the bomb entity exists and
            // points at its owner whether it is held or slung.
            static uintptr_t s_carrier = 0;

            const uint32_t hw =
                mem.read<uint32_t>(ws + offsets::m_hActiveWeapon);
            if (!hw || hw == 0xFFFFFFFFu) continue;

            const uintptr_t w = resolve_handle(
                mem, el, c_.chunk_off, c_.slot_stride, hw);
            if (!valid_ptr(w)) continue;

            const uint16_t id = item_def_index(mem, w);
            if (id == offsets::kItemDefC4) {
                t.has_bomb = true;
                s_carrier = t.pawn;
                std::snprintf(t.weapon, sizeof(t.weapon), "C4");
                continue;
            }
            if (t.pawn == s_carrier) {
                // Not holding it any more, but may still be carrying it.
                t.has_bomb = true;
            }

            const char* nm = weapon_name(id);
            if (nm) {
                std::snprintf(t.weapon, sizeof(t.weapon), "%s", nm);
            } else {
                char dn[24] = {};
                if (read_designer(mem, w, dn, sizeof(dn))) {
                    const char* pretty = pretty_from_designer(dn);
                    std::snprintf(t.weapon, sizeof(t.weapon), "%s",
                                  pretty ? pretty : dn);
                } else {
                    std::snprintf(t.weapon, sizeof(t.weapon), "#%d", id);
                }
            }
        }
    }

    // ---- planted C4 ----
    bool bomb_on = false;
    Vec3 bomb{};

    uintptr_t c4 = 0;

    const uintptr_t pl =
        mem.read<uintptr_t>(client_base + offsets::dwPlantedC4);
    if (looks_like_entity(mem, pl)) {
        c4 = pl;
    } else if (valid_ptr(pl)) {
        const uintptr_t inner = mem.read<uintptr_t>(pl);
        if (looks_like_entity(mem, inner)) c4 = inner;
    }

    if (!c4 && g_defidx_off && !pawns.empty()) {
        static std::vector<uintptr_t> c4_slots;
        static double c4_scan_at = -1e9;

        if (now - c4_scan_at > 1000.0) {
            c4_scan_at = now;
            c4_slots.clear();

            for (int ch = 0; ch < offsets::kChunks && c4_slots.size() < 4; ++ch) {
                const uintptr_t cp =
                    mem.read<uintptr_t>(el + c_.chunk_off + 8 * ch);
                if (!valid_ptr(cp)) continue;
                for (int i = 1; i < offsets::kSlots; ++i) {
                    const uintptr_t e =
                        mem.read<uintptr_t>(cp + c_.slot_stride * i);
                    if (!valid_ptr(e)) continue;
                    if (item_def_index(mem, e) != offsets::kItemDefC4) continue;
                    c4_slots.push_back(e);
                    break;
                }
            }
        }

        if (!c4_slots.empty()) c4 = c4_slots[0];
    }

    diag_c4_ent = c4;

    if (c4) {
        const uintptr_t node =
            mem.read<uintptr_t>(c4 + offsets::m_pGameSceneNode);

        static const C4Cand kCand[] = {
            { true,  offsets::m_vecAbsOrigin },
            { true,  0xD0 }, { true, 0xC8 }, { true, 0xD8 },
            { false, offsets::m_vOldOrigin },
        };

        auto accept = [&](const Vec3& v) {
            if (!sane_vec(v)) return false;
            const float dx = v.x - local_origin.x;
            const float dy = v.y - local_origin.y;
            const float dz = v.z - local_origin.z;
            return dx * dx + dy * dy + dz * dz < 12000.0f * 12000.0f;
        };

        auto read_pos = [&](const C4Cand& cd, Vec3& v) {
            const uintptr_t base = cd.on_node ? node : c4;
            v.x = mem.read<float>(base + cd.off);
            v.y = mem.read<float>(base + cd.off + 4);
            v.z = mem.read<float>(base + cd.off + 8);
            return sane_vec(v);
        };

        if (c_.c4_ok) {
            Vec3 v{};
            if (read_pos(c_.c4_cand, v) && accept(v)) { bomb = v; bomb_on = true; }
            else c_.c4_ok = false;
        }

        if (!c_.c4_ok) {
            for (const C4Cand& cd : kCand) {
                Vec3 v{};
                if (!read_pos(cd, v) || !accept(v)) continue;

                const bool same = (c_.c4_cand.on_node == cd.on_node &&
                                   c_.c4_cand.off == cd.off);
                if (same && c_.c4_last_t > 0.0 &&
                    std::fabs(v.z - c_.c4_last.z) < 0.2f) {
                    ++c_.c4_stable;
                } else {
                    c_.c4_cand = cd;
                    c_.c4_stable = 1;
                }
                c_.c4_last = v;
                c_.c4_last_t = now;

                if (c_.c4_stable >= 4) {
                    c_.c4_ok = true; bomb = v; bomb_on = true; break;
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
            const float f = (span > 0.0001) ? (float)((rt - b.t) / span) : 0.0f;
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
        e.pawn      = t.pawn;      // needed by the triggerbot's vis check
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
