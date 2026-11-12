#include "mission.h"

#include <cstdio>
#include <cstdlib>

#include <algorithm>
#include <cstring>

namespace hg2 {

namespace {

// Block: u32 crc; u32 size (counts itself); payload. Returns a reader limited to the block
// positioned after the size field.
Reader openBlock(Reader& r) {
    r.u32();  // crc
    const uint8_t* start = r.p;
    uint32_t size = r.u32();
    Reader b(start, size);
    b.p = start + 4;
    if (start + size > r.end) b.end = r.end;
    r.p = start + size;
    return b;
}

void parseWorldStatus(Reader b, Mission& m) {
    b.u32();  // '{'
    for (uint32_t t = b.u32(); t != 1 && b.ok(); t = b.u32()) {
        switch (t) {
            case 0x1d: m.fogMin = b.f32(); break;
            case 0x1e: m.fogMax = b.f32(); break;
            case 0x1f: m.fogIntensity = std::min(1.f, std::max(0.f, b.f32())); break;
            case 0x20: case 0x21: case 0x22: case 0x23: m.fog[t - 0x20] = uint8_t(b.u32()); break;
            case 0x24: m.clipping = b.i32(); break;
            case 0x25: m.lightAmbient = b.f32(); break;
            case 0x26: m.lightDiffuse = b.f32(); break;
            case 0x27: case 0x28: case 0x29: m.lightDir[t - 0x27] = b.f32(); break;
            case 0x2a: case 0x2b: case 0x2c: m.lightColor[t - 0x2a] = uint8_t(b.u32()); break;
            case 0x39: m.musicTrack = b.i32(); break;  // RedBook_Track (CD audio track)
            default:
                if (getenv("HG2_WSDBG")) fprintf(stderr, "world status token %x\n", t);
                return;
        }
    }
}

void parseWater(Reader b, Mission& m) {
    m.hasWater = true;
    b.u32();
    for (uint32_t t = b.u32(); t != 1 && b.ok(); t = b.u32()) {
        switch (t) {
            case 0x2d: m.waterLevel = b.f32(); break;
            case 0x2e: m.waterAlpha = b.i32(); break;
            case 0x2f: m.waterTidal = b.f32(); break;
            case 0x30: m.waterAnim = b.f32(); break;
            case 0x44: m.waterTexture = b.str(); break;
            default: return;
        }
    }
}

void parseSky(Reader b, Mission& m) {
    b.u32();
    for (uint32_t t = b.u32(); t != 1 && b.ok(); t = b.u32()) {
        if (t == 0x44) m.skyTexture = b.str();
        else if (t == 0x45) m.skyModel = b.str();
        else return;
    }
}

void parseScripting(Reader b, Mission& m) {
    b.u32();
    for (uint32_t t = b.u32(); t != 1 && b.ok(); t = b.u32()) {
        if (t == 0x34) m.scriptProjects.push_back(b.str());
        else if (t == 0x35) m.scriptLibraries.push_back(b.str());
        else if (t == 0xa1) m.scriptFiles.push_back(b.str());
        else return;
    }
}

bool parseObject(Reader& r, bool user, MissionObject& o) {
    o.userHeader = user;
    int32_t a = r.i32();
    r.i32();
    if (a >= 0 && user) r.u32();
    Reader b = openBlock(r);
    b.u32();  // '{'
    for (uint32_t t = b.u32(); t != 1 && b.ok(); t = b.u32()) {
        switch (t) {
            case 0xa1: o.script = b.str(); break;
            case 0x14f: o.walkGraph = b.str(); break;
            case 0x3e: o.name = b.str(); break;
            case 0x43: {  // Light_Information: '{' RGB u32[3], f32 range, f32[3], f32 cone in/out, f32 intensity, u32[3] '}'
                b.u32();
                for (auto& c : o.lightColor) c = uint8_t(std::min<uint32_t>(255, b.u32()));
                o.lightRange = b.f32();
                b.skip(5 * 4);
                o.lightIntensity = b.f32();
                b.skip(3 * 4);
                b.u32();
                o.isLight = true;
                break;
            }
            case 0x3d: {  // Object_World_State
                b.u32();
                for (double& d : o.pos) d = b.f64();
                for (double& d : o.rot) d = b.f64();
                if (b.u32() == 0x42) {
                    for (double& d : o.quat) d = b.f64();
                    o.hasQuat = true;
                    b.u32();
                }
                break;
            }
            case 0x3c: {  // Object_definition_file
                b.u32();
                o.type = b.i32();
                o.defIndex = b.i32();
                int32_t team = b.i32();
                if (team >= 99 && team <= 0x6b) {
                    o.team = team;
                    b.u32();
                }
                break;
            }
            default: return b.p <= b.end;
        }
    }
    return true;
}

// First C string in the block ending with the given extension (".m", ".ecy").
std::string firstWithExt(const uint8_t* p, size_t n, const char* ext) {
    size_t el = strlen(ext);
    for (size_t i = 0; i + el < n; i++) {
        if (p[i + el] != 0) continue;
        bool match = true;
        for (size_t k = 0; k < el && match; k++) match = tolower(p[i + k]) == ext[k];
        if (!match) continue;
        size_t s = i;
        while (s > 0 && p[s - 1] >= 33 && p[s - 1] < 127) s--;
        if (i - s >= 3) return std::string(reinterpret_cast<const char*>(p + s), i + el - s);
    }
    return "";
}

// 0xcb Path (FUN_1008d160): 0xcc Name STR, 0xcf Rep_File STR, 0xcd Anchor f32×3, 0xce Control_Point f32×3
// (relative to the anchor), 0xd0 Line / 0xd1 Bezier segments { 0xcc name, 0xce × 4, '}' }, ended by '}'.
bool parsePath(Reader& r, MissionPath& p) {
    // Nodes (0xce) are offsets from the anchor; each segment (0xd0 line / 0xd1 cubic) lists control points as
    // offsets from the anchor too, but they describe the curve from node k to node k+1 relative to node k.
    std::vector<std::array<float, 3>> nodes;
    std::vector<std::vector<std::array<float, 3>>> segs;
    for (uint32_t t = r.u32(); t != 1 && r.ok(); t = r.u32()) {
        switch (t) {
            case 0xcc: p.name = r.str(); break;
            case 0xcf: r.str(); break;
            case 0xcd: for (float& f : p.anchor) f = r.f32(); break;
            case 0xce: {
                float x = r.f32(), y = r.f32(), z = r.f32();
                nodes.push_back({p.anchor[0] + x, p.anchor[1] + y, p.anchor[2] + z});
                break;
            }
            case 0xd0:
            case 0xd1: {
                std::vector<std::array<float, 3>> cp;
                for (uint32_t k = r.u32(); k != 1 && r.ok(); k = r.u32()) {
                    if (k == 0xcc) r.str();
                    else if (k == 0xce) {
                        float x = r.f32(), y = r.f32(), z = r.f32();
                        cp.push_back({x, y, z});
                    } else if (k == 0xd0 || k == 0xd1 || k == 0xcb) {
                        r.p -= 4;  // segment without its own terminator: the next one starts here
                        break;
                    } else {
                        if (getenv("HG2_PATHDBG")) fprintf(stderr, "path %s: segment token %x\n", p.name.c_str(), k);
                        return false;
                    }
                }
                if (t == 0xd0) cp.clear();  // straight segment
                segs.push_back(std::move(cp));
                break;
            }
            case 0xcb: case 0x3a: case 0x3b: case 0x12a: case 0xd2: case 0x157: case 0xdf: case 0:
                r.p -= 4;  // path without a terminator: the next top-level block starts here
                t = 1;
                goto done;
            default:
                if (getenv("HG2_PATHDBG")) fprintf(stderr, "path %s: token %x\n", p.name.c_str(), t);
                return false;
        }
    }
done:
    // Segments come as (start node, end node, curve): node pairs 2k / 2k+1 belong to segment k.
    auto add = [&](const std::array<float, 3>& q) {
        if (p.points.empty() || q != p.points.back()) p.points.push_back(q);
    };
    if (segs.empty() || nodes.size() < 2 * segs.size()) {  // plain list of points
        for (auto& n : nodes) add(n);
        return true;
    }
    for (size_t i = 0; i < segs.size(); i++) {
        const auto &a0 = nodes[2 * i], &a1 = nodes[2 * i + 1];
        add(a0);
        if (segs[i].size() == 4) {
            std::array<std::array<float, 3>, 4> c;
            for (int j = 0; j < 4; j++)
                for (int k = 0; k < 3; k++) c[size_t(j)][size_t(k)] = a0[size_t(k)] + segs[i][size_t(j)][size_t(k)];
            for (int sm = 1; sm < 8; sm++) {
                float u = float(sm) / 8.f, a = (1 - u) * (1 - u) * (1 - u), b = 3 * u * (1 - u) * (1 - u),
                      cc = 3 * u * u * (1 - u), d = u * u * u;
                std::array<float, 3> q;
                for (int k = 0; k < 3; k++)
                    q[size_t(k)] = a * c[0][size_t(k)] + b * c[1][size_t(k)] + cc * c[2][size_t(k)] + d * c[3][size_t(k)];
                add(q);
            }
        }
        add(a1);
    }
    for (size_t i = 2 * segs.size(); i < nodes.size(); i++) add(nodes[i]);
    return true;
}

// Surfaces block: crc, size, '{', 0xd3 count, then entries 0xd6 {0xd4 index, 0xd5 r g b, 0xd7 f32 friction,
// 0xd8 u32 damage, 0x154 u32, 0x153 u32, 0x155 STR footstep, 0x156 r g b} each closed by '}'.
bool parseSurfaces(Reader b, Mission& m) {
    b.u32();  // '{'
    for (uint32_t t = b.u32(); t != 1 && b.ok(); t = b.u32()) {
        if (t == 0xd3) {
            b.u32();
        } else if (t == 0xd6) {
            Surface s;
            for (uint32_t k = b.u32(); k != 1 && b.ok(); k = b.u32()) {
                switch (k) {
                    case 0xd4: s.index = b.i32(); break;
                    case 0xd5: for (auto& c : s.color) c = uint8_t(b.u32()); break;
                    case 0xd7: s.friction = b.f32(); break;
                    case 0xd8: s.damage = b.i32(); break;
                    case 0x153: case 0x154: b.u32(); break;
                    case 0x155: s.footstep = b.str(); break;
                    case 0x156: b.u32(), b.u32(), b.u32(); break;
                    default: return false;
                }
            }
            m.surfaces.push_back(s);
        } else {
            return false;
        }
    }
    return true;
}

bool parseFountains(Reader& r, Mission& m) {
    if (r.u32() != 2) return false;  // '{'
    for (uint32_t t = r.u32(); t != 1; t = r.u32()) {
        if (!r.ok()) return false;
        if (t == 0x12d) {  // template
            if (r.u32() != 2) return false;
            FountainTemplate ft;
            for (uint32_t k = r.u32(); k != 1; k = r.u32()) {
                if (!r.ok()) return false;
                if (k == 0x3c) ft.defIndex = r.i32();
                else if (k == 0x3f) ft.type = r.i32();
                else if (k == 0xa1) ft.script = r.str();
                else return false;
            }
            m.fountainTemplates.push_back(ft);
        } else if (t == 0x12e) {  // fountain
            if (r.u32() != 2) return false;
            Fountain f;
            for (uint32_t k = r.u32(); k != 1; k = r.u32()) {
                if (!r.ok()) return false;
                switch (k) {
                    case 0x3e: f.name = r.str(); break;
                    case 0xa1: f.script = r.str(); break;
                    case 0x6c: f.team = r.i32(); break;
                    case 0x132: f.active = r.i32() == 3; break;
                    case 0x133: r.u32(); break;
                    case 0x12f: f.mode = r.i32(); break;
                    case 0x130: f.total = r.i32(); break;
                    case 0x131: f.maxAlive = std::max(1, std::min(8, r.i32())); break;
                    case 0x12b: f.wave = std::max(1, std::min(16, r.i32())); break;
                    case 0x134: f.flags = r.u32(); break;
                    case 0x40: for (float& v : f.pos) v = r.f32(); break;
                    case 0x41: for (float& v : f.rot) v = r.f32(); break;
                    case 0x15e: f.range[0] = r.f32(); break;
                    case 0x15f: f.range[1] = r.f32(); break;
                    case 0x160: f.range[2] = r.f32(); break;
                    case 0x135: f.templates.push_back(r.i32()); break;
                    default: return false;
                }
            }
            m.fountains.push_back(f);
        } else {
            r.u32();  // system-level scalars (0x132 active, 0x12b template count, 0x12c pool size)
        }
    }
    return true;
}

}  // namespace

int Mission::addDef(const uint8_t* data, size_t size) {
    auto b = std::make_shared<Bytes>(data, data + size);
    extraDefData.push_back(b);
    MissionDef d;
    d.data = b->data();
    d.size = b->size();
    d.firstModel = firstWithExt(d.data, d.size, ".m");
    d.animScript = firstWithExt(d.data, d.size, ".ecy");
    defs.push_back(d);
    return int(defs.size()) - 1;
}

const MissionPath* Mission::path(const std::string& name) const {
    for (auto& p : paths)
        if (lower(p.name) == lower(name)) return &p;
    return nullptr;
}

bool loadMission(const Bytes& data, Mission& m, std::string* err) {
    m.raw = data;
    Reader r(m.raw);
    if (!r.ok(40)) return false;
    uint32_t crc = r.u32();
    uint32_t size = r.u32();
    if (size + 4 > m.raw.size() || crc32hg(m.raw.data() + 4, size) != crc) {
        if (err) *err = "mission checksum mismatch";
        return false;
    }
    for (int i = 0; i < 6; i++) r.u32();
    r.str();
    uint32_t n1 = r.u32();
    for (uint32_t i = 0; i < n1 && r.ok(4); i++) r.skip(r.u32());
    uint32_t n2 = r.u32();
    for (uint32_t i = 0; i < n2 && r.ok(4); i++) {
        uint32_t sz = r.u32();
        MissionDef d;
        d.data = r.p;
        d.size = sz;
        d.firstModel = firstWithExt(r.p, std::min<size_t>(sz, r.left()), ".m");
        d.animScript = firstWithExt(r.p, std::min<size_t>(sz, r.left()), ".ecy");
        m.defs.push_back(d);
        r.skip(sz);
    }
    for (uint32_t t = r.u32(); t != 0 && r.ok(); t = r.u32()) {
        switch (t) {
            case 0x18:
                m.title = r.str();
                m.textFile = r.str();
                r.skip(0x41);
                r.u32();
                r.u32();
                break;
            case 0x1c: parseWorldStatus(openBlock(r), m); break;
            case 0x31: parseWater(openBlock(r), m); break;
            case 0x32: parseSky(openBlock(r), m); break;
            case 0x36: parseScripting(openBlock(r), m); break;
            case 0x33: m.terrain = r.str(); break;
            case 0x3a:
            case 0x3b: {
                MissionObject o;
                if (!parseObject(r, t == 0x3a, o)) {
                    if (err) *err = "bad object block";
                    return false;
                }
                m.objects.push_back(std::move(o));
                break;
            }
            case 0xdf: {
                r.u32();
                for (uint32_t k = r.u32(); k != 1 && r.ok(); k = r.u32()) {}
                break;
            }
            case 0x157: m.msgFile = r.str(); break;
            case 0xd2: parseSurfaces(openBlock(r), m); break;
            case 0x12a: {  // FountainSystem
                const uint8_t* save = r.p;
                if (!parseFountains(r, m)) {
                    r.p = save;  // fall back to resynchronising below
                    static const uint8_t sig[8] = {0xcb, 0, 0, 0, 0xcc, 0, 0, 0};
                    const uint8_t* q = std::search(r.p, r.end, sig, sig + 8);
                    if (q == r.end) return true;
                    r.p = q;
                }
                break;
            }
            case 0xcb: {
                MissionPath p;
                if (!parsePath(r, p)) return true;
                if (!p.points.empty()) m.paths.push_back(std::move(p));
                break;
            }
            default: {
                // 0xd2 Surfaces and 0x12a FountainSystem are not decoded yet: resynchronise on the next path
                static const uint8_t sig[8] = {0xcb, 0, 0, 0, 0xcc, 0, 0, 0};
                const uint8_t* q = std::search(r.p, r.end, sig, sig + 8);
                if (q == r.end) return true;
                r.p = q;
                break;
            }
        }
    }
    return true;
}

}  // namespace hg2
