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
// the exact path the earlier working build used, so a refused bulk read can
// never be what empties the player list.
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
// *** THE BUG THAT EMPTIED THE LIST ***
// This used to read each 61 KB chunk in fixed 16 KB pieces, then step by the
// slot stride from each piece start. But 16384 is not a multiple of the 120
// byte stride (16384/120 = 136.5), so only the FIRST piece landed on real slot
// boundaries -- every later piece read from 64 bytes inside a slot and
// returned noise. Pawns past slot 136 were therefore never seen at all.
//
// The fix is to make each read exactly a whole number of slots.
void enumerate_slots(const Memory& mem, uintptr_t el, uintptr_t chunk_off,
                     uintptr_t stride, std::vector<uintptr_t>& out,
                     bool& used_bulk) {
    out.clear();
    used_bulk = false;
    if (!stride) return;

    constexpr size_t kSlotsPerRead = 128;           // 128 * 120 = 15 KB
    const size_t piece_bytes = kSlotsPerRead * stride;

    static std::vector<uint8_t> buf;
    if (buf.size() < piece_bytes) buf.resize(piece_bytes);

    for (int ch = 0; ch < offsets::kChunks; ++ch) {
        const uintptr_t base = mem.read<uintptr_t>(el + chunk_off + 8 * ch);
        if (!valid_ptr(base)) continue;

        std::vector<uintptr_t> local;
        bool ok = true;

        for (size_t s0 = 0; s0 < static_cast<size_t>(offsets::kSlots);
             s0 += kSlotsPerRead) {
            const size_t n_slots =
                (std::min)(kSlotsPerRead,
                           static_cast<size_t>(offsets::kSlots) - s0);
            const size_t bytes = n_slots * stride;

            // Always a whole number of slots from a slot-aligned start.
            if (!mem.read_bytes(base + s0 * stride, buf.data(), bytes)) {
                ok = false;
                break;
            }
            for (size_t k = 0; k < n_slots; ++k) {
                uintptr_t e = 0;
                std::memcpy(&e, buf.data() + k * stride, 8);
                if (valid_ptr(e)) local.push_back(e);
            }
        }

        if (ok) {
            used_bulk = true;
            out.insert(out.end(), local.begin(), local.end());
        } else {
            for (int i = 0; i < offsets::kSlots; ++i) {
                const uintptr_t e = mem.read<uintptr_t>(base + stride * i);
                if (valid_ptr(e)) out.push_back(e);
            }
        }
    }
}

// Slots -> live player pawns, with staged counters so a failure is visible.
void collect_pawns(const Memory& mem, uintptr_t el, uintptr_t chunk_off,
                   uintptr_t stride, std::vector<uintptr_t>& all,
                   std::vector<uintptr_t>& hp, std::vector<uintptr_t>& team_ok,
                   bool& bulk) {
    enumerate_slots(mem, el, chunk_off, stride, all, bulk);

    hp.clear();
    team_ok.clear();
    for (uintptr_t e : all) {
        const EntHead h = read_ent_head(mem, e);
        if (h.health <= 0 || h.health > 100) continue;
        hp.push_back(e);
        if (h.team == 2 || h.team == 3) team_ok.push_back(e);
    }
}

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

// A plausible player name: printable, short, and not a run of one character.
bool name_looks_ok(const char* s) {
    size_t n = 0;
    for (; n < 32 && s[n]; ++n) {
        const unsigned char ch = static_cast<unsigned char>(s[n]);
        if (ch < 32 || ch >= 127) return false;
    }
    if (n < 2) return false;
    bool varied = false;
    for (size_t i = 1; i < n; ++i)
        if (s[i] != s[0]) { varied = true; break; }
    return varied;
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

// ── weapon id -> name ────────────────────────────────────────────────────
const char* weapon_name(int id) {
    switch (id) {
        case 1:  return "Deagle";     case 2:  return "Dual Berettas";
        case 3:  return "Five-SeveN"; case 4:  return "Glock-18";
        case 7:  return "AK-47";      case 8:  return "AUG";
        case 9:  return "AWP";        case 10: return "FAMAS";
        case 11: return "G3SG1";      case 13: return "Galil AR";
        case 14: return "M249";       case 16: return "M4A4";
        case 17: return "MAC-10";     case 19: return "P90";
        case 23: return "MP5-SD";     case 24: return "UMP-45";
        case 25: return "XM1014";     case 26: return "PP-Bizon";
        case 27: return "MAG-7";      case 28: return "Negev";
        case 29: return "Sawed-Off";  case 30: return "Tec-9";
        case 31: return "Zeus x27";   case 32: return "P2000";
        case 33: return "MP7";        case 34: return "MP9";
        case 35: return "Nova";       case 36: return "P250";
        case 38: return "SCAR-20";    case 39: return "SG 553";
        case 40: return "SSG 08";     case 43: return "Flashbang";
        case 44: return "HE Grenade"; case 45: return "Smoke";
        case 46: return "Molotov";    case 47: return "Decoy";
        case 48: return "Incendiary"; case 49: return "C4";
        case 60: return "M4A1-S";     case 61: return "USP-S";
        case 63: return "CZ75-Auto";  case 64: return "R8 Revolver";
        default: break;
    }
    if (id >= 500 && id <= 600) return "Knife";
    return nullptr;
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
    static int    refresh_n = 0;

    // ── refresh the pawn list ~10x/sec, auto-detecting the list layout ────
    if (now - slots_at > 100.0) {
        slots_at = now;

        struct Layout { uintptr_t off, stride; };
        static const Layout kLayouts[] = {
            {0x10, 120}, {0x10, 112}, {0x08, 120}, {0x08, 112},
        };

        int best = -1;
        for (const Layout& L : kLayouts) {
            std::vector<uintptr_t> all, hp, tm;
            bool bulk = false;
            collect_pawns(mem, el, L.off, L.stride, all, hp, tm, bulk);

            if (static_cast<int>(hp.size()) > best) {
                best = static_cast<int>(hp.size());
                c_.chunk_off   = L.off;
                c_.slot_stride = L.stride;
                slots = std::move(all);
                // Prefer the team-filtered set; if the team offset is stale and
                // nothing matched, fall back to health-only so boxes still draw.
                pawns = tm.empty() ? hp : tm;
                diag_slots = static_cast<int>(slots.size());
                diag_hp    = static_cast<int>(hp.size());
                diag_team  = static_cast<int>(tm.size());
                diag_bulk  = bulk;
            }
        }
        if (best <= 0) pawns.clear();

        ++refresh_n;

        // ── names / weapons: slow-moving, refresh a couple of times a second
        if (refresh_n % 5 == 0 && !pawns.empty()) {
            // controller -> pawn handle. Prefer the schema value, then vote.
            if (!c_.ctrl_ok) {
                static const uintptr_t cand[] = {
                    0x90C, 0x8FC, 0x908, 0x900, 0x910, 0x914,
                    0x918, 0x91C, 0x920, 0x924, 0x8F8, 0x904,
                };
                int best_v = 0;
                for (uintptr_t off : cand) {
                    int score = 0;
                    for (uintptr_t s : slots) {
                        const uint32_t h = mem.read<uint32_t>(s + off);
                        if (!h || h == 0xFFFFFFFFu) continue;
                        const uintptr_t p = resolve_handle(
                            mem, el, c_.chunk_off, c_.slot_stride, h);
                        if (p && std::find(pawns.begin(), pawns.end(), p) !=
                                 pawns.end())
                            ++score;
                    }
                    if (score > best_v) { best_v = score; c_.ctrl_link = off; }
                }
                c_.ctrl_ok = best_v >= 1;
            }

            // name offset: pick whichever candidate yields a real-looking name
            // for the most controllers.
            if (c_.ctrl_ok && !c_.name_ok) {
                static const uintptr_t name_c[] = {
                    0x6F4, 0x6E8, 0x700, 0x6E0, 0x6C8, 0x708, 0x710, 0x6D8,
                };
                int best_v = 0;
                for (uintptr_t off : name_c) {
                    int score = 0;
                    for (uintptr_t s : slots) {
                        const uint32_t h = mem.read<uint32_t>(s + c_.ctrl_link);
                        if (!h || h == 0xFFFFFFFFu) continue;
                        const uintptr_t ctrl = resolve_handle(
                            mem, el, c_.chunk_off, c_.slot_stride, h);
                        if (!ctrl) continue;
                        char raw[32] = {};
                        if (!mem.read_bytes(ctrl + off, raw, sizeof(raw)))
                            continue;
                        if (name_looks_ok(raw)) ++score;
                    }
                    if (score > best_v) { best_v = score; c_.name_off = off; }
                }
                c_.name_ok = best_v >= 2;
            }

            // weapon chain: services ptr -> active weapon handle -> defindex.
            // Graded by whether the id is a real weapon, so a wrong candidate
            // can't win.
            if (!c_.weapon_ok && pawns.size() >= 2) {
                static const uintptr_t svc_c[] = {
                    0x13F0, 0x11E0, 0x11D8, 0x11E8, 0x13D8, 0x13E0, 0x1200,
                };
                static const uintptr_t act_c[] = { 0x58, 0x60, 0x50 };
                static const uintptr_t attr_c[] = { 0x1180, 0x1378, 0x1370 };

                int best_v = 0;
                for (uintptr_t so : svc_c) {
                    for (uintptr_t ao : act_c) {
                        for (uintptr_t at : attr_c) {
                            int score = 0;
                            for (uintptr_t p : pawns) {
                                const uintptr_t svc = mem.read<uintptr_t>(p + so);
                                if (!valid_ptr(svc)) continue;
                                const uint32_t h =
                                    mem.read<uint32_t>(svc + ao);
                                const uintptr_t w = resolve_handle(
                                    mem, el, c_.chunk_off, c_.slot_stride, h);
                                if (!valid_ptr(w)) continue;
                                const uint16_t id = mem.read<uint16_t>(
                                    w + at + 0x50 + 0x1BA);
                                const bool plausible =
                                    (id >= 1 && id <= 90) ||
                                    (id >= 500 && id <= 600);
                                if (plausible) ++score;
                            }
                            if (score > best_v) {
                                best_v = score;
                                c_.wservices = so;
                                c_.wactive   = ao;
                                c_.attrmgr   = at;
                            }
                        }
                    }
                }
                c_.weapon_ok = best_v >= 1;
            }

            // pull name + weapon per pawn
            for (uintptr_t p : pawns) {
                for (Track& t : tracks_) {
                    if (t.pawn != p) continue;

                    if (c_.ctrl_ok && c_.name_ok) {
                        for (uintptr_t s : slots) {
                            const uint32_t h =
                                mem.read<uint32_t>(s + c_.ctrl_link);
                            if (!h || h == 0xFFFFFFFFu) continue;
                            const uintptr_t ctrl = resolve_handle(
                                mem, el, c_.chunk_off, c_.slot_stride, h);
                            if (ctrl != p) continue;
                            char raw[32] = {};
                            if (mem.read_bytes(ctrl + c_.name_off, raw,
                                               sizeof(raw)))
                                sanitize(t.name, sizeof(t.name), raw,
                                         sizeof(raw));
                            break;
                        }
                    }

                    if (c_.weapon_ok) {
                        const uintptr_t svc = mem.read<uintptr_t>(p + c_.wservices);
                        if (valid_ptr(svc)) {
                            const uint32_t h =
                                mem.read<uint32_t>(svc + c_.wactive);
                            const uintptr_t w = resolve_handle(
                                mem, el, c_.chunk_off, c_.slot_stride, h);
                            if (valid_ptr(w)) {
                                const uint16_t id = mem.read<uint16_t>(
                                    w + c_.attrmgr + 0x50 + 0x1BA);
                                const char* nm = weapon_name(id);
                                if (nm)
                                    std::snprintf(t.weapon, sizeof(t.weapon),
                                                  "%s", nm);
                            }
                        }
                    }
                    break;
                }
            }
        }
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
        if (h.health <= 0 || h.health > 100) continue;

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

        Track moved = *dst;      // copies history + name + weapon
        moved.health    = h.health;
        moved.team      = h.team;
        moved.distance  = std::sqrt(dx * dx + dy * dy + dz * dz) / 40.0f;
        moved.last_seen = now;
        moved.push(s);
        fresh.push_back(std::move(moved));
    }

    if (c_.bones_ok && !pawns.empty() && bone_hits == 0) ++c_.bone_fail;
    else                                                c_.bone_fail = 0;

    // ── bomb: planted C4 ────────────────────────────────────────────────
    bool  bomb_active = false;
    Vec3  bomb_origin{};
    float bomb_timer = -1.0f;

    const uintptr_t planted =
        mem.read<uintptr_t>(client_base + offsets::dwPlantedC4);
    if (valid_ptr(planted)) {
        const uintptr_t node =
            mem.read<uintptr_t>(planted + offsets::m_pGameSceneNode);
        if (valid_ptr(node)) {
            if (!c_.origin_ok) {
                for (uintptr_t off = 0x40; off <= 0x180; off += 4) {
                    Vec3 v{};
                    v.x = mem.read<float>(node + off);
                    v.y = mem.read<float>(node + off + 4);
                    v.z = mem.read<float>(node + off + 8);
                    if (!sane_vec(v)) continue;
                    const float d = std::sqrt(
                        (v.x - local_origin.x) * (v.x - local_origin.x) +
                        (v.y - local_origin.y) * (v.y - local_origin.y) +
                        (v.z - local_origin.z) * (v.z - local_origin.z));
                    if (d > 8000.0f) continue;
                    c_.node_origin = off;
                    c_.origin_ok = true;
                    break;
                }
            }
            if (c_.origin_ok) {
                bomb_origin.x = mem.read<float>(node + c_.node_origin);
                bomb_origin.y = mem.read<float>(node + c_.node_origin + 4);
                bomb_origin.z = mem.read<float>(node + c_.node_origin + 8);
                if (sane_vec(bomb_origin)) {
                    bomb_active = true;
                    if (!c_.timer_ok) {
                        for (uintptr_t off = 0x900; off <= 0x1400; off += 4) {
                            const float v = mem.read<float>(planted + off);
                            if (v > 0.1f && v <= 45.0f) {
                                c_.timer_off = off;
                                c_.timer_ok  = true;
                                break;
                            }
                        }
                    }
                    if (c_.timer_ok)
                        bomb_timer = mem.read<float>(planted + c_.timer_off);
                }
            }
        }
    }

    std::lock_guard<std::mutex> lk(mtx);
    tracks_       = std::move(fresh);
    bomb_active_  = bomb_active;
    bomb_origin_  = bomb_origin;
    bomb_timer_   = bomb_timer;
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
    b.timer    = bomb_timer_;
    b.active   = true;
    return b;
}
