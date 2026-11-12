#include "gear.h"

namespace hg2 {

namespace {
// Nested block: u32 crc; u32 size (counts itself); '{' ... '}'. Returns reader over the payload after '{'.
Reader block(Reader& r) {
    r.u32();
    const uint8_t* start = r.p;
    uint32_t size = r.u32();
    Reader b(start, size);
    if (start + size > r.end) b.end = r.end;
    b.p = start + 4;
    b.u32();  // '{'
    r.p = start + size;
    return b;
}
}  // namespace

bool parseGearDef(const uint8_t* p, size_t n, GearDef& g) {
    Reader r(p, n);
    for (uint32_t t = r.u32(); t != 0 && r.ok(); t = r.u32()) {
        switch (t) {
            case 0x15a: g.targetingId = r.i32(); break;
            case 0xa0: g.animScript = r.str(); break;
            case 0xa1: g.scriptFile = r.str(); break;
            case 0xb2: g.detailMap = r.str(); break;
            case 0xad: g.hitPoints = r.i32(); break;
            case 0x9c: {  // Geometry_Files
                Reader b = block(r);
                g.geometry.push_back(b.str());
                break;
            }
            case 0x9d: {  // Alternate_Geometry
                Reader b = block(r);
                for (uint32_t k = b.u32(); k != 1 && b.ok(); k = b.u32()) {
                    if (k == 0x9f) g.maxRadius = b.f32();
                    else if (k == 0x9e) g.geometry.push_back(b.str());
                    else break;
                }
                break;
            }
            case 0x88: {  // Detection
                Reader b = block(r);
                for (uint32_t k = b.u32(); k != 1 && b.ok(); k = b.u32()) {
                    if (k >= 0x89 && k <= 0x8e) g.detection[k - 0x89] = b.f32();
                    else break;
                }
                break;
            }
            case 0x8f: {  // Stats
                Reader b = block(r);
                for (uint32_t k = b.u32(); k != 1 && b.ok(); k = b.u32()) {
                    switch (k) {
                        case 0x90: g.sensors = b.i32(); break;
                        case 0x91: g.fireControl = b.i32(); break;
                        case 0x92: g.armor = b.i32(); break;
                        case 0x93: g.size = b.i32(); break;
                        case 0x94: g.maneuverability = b.i32(); break;
                        case 0x95: g.movement1 = b.i32(); break;
                        case 0x96: g.movement2 = b.i32(); break;
                        case 0x97: g.maxWalkSpeed = b.f32(); break;
                        case 0x98: g.maxRunSpeed = b.f32(); break;
                        case 0x99: case 0x9a: b.f32(); break;
                        case 0x9b: g.perks = b.u32(), g.flaws = b.u32(); break;
                        default: k = 1; b.p = b.end; break;
                    }
                }
                break;
            }
            case 0xa2: {  // Hard_Point: '{' Frame '}' then HPn weapon pairs
                Reader b = block(r);
                for (uint32_t k = b.u32(); b.ok() && k != 0; k = b.u32()) {
                    if (k >= 0xa4 && k <= 0xac && b.ok(8)) {
                        int w = b.i32(), ammo = b.i32();
                        g.hardpoints.push_back({int(k - 0xa4 + 1), w, ammo});
                    }
                }
                break;
            }
            case 0xa3: {  // Sound_File
                Reader b = block(r);
                for (uint32_t k = b.u32(); k != 1 && b.ok(); k = b.u32()) {
                    std::string s = b.str();
                    if (k == 0xaf) g.soundMotor = s;
                    else if (k == 0xb0) g.soundSteps = s;
                    else if (k == 0xae) g.soundDestruction = s;
                    else if (k == 0xad) g.soundDamage = s;
                    else if (k == 0xb1) g.soundWheels = s;
                }
                break;
            }
            case 0x85: {  // Damage_Hierarchy: {tok, u32} entries until '}' (FUN_1004bcf0)
                Reader b = block(r);
                for (int guard = 0; b.ok(8) && guard < 512; guard++) {
                    uint32_t k = b.u32();
                    b.u32();
                    if (k == 1) break;
                    if (k == 0x86) {
                        GearDef::DamagePart d;
                        d.frame = b.str();
                        d.system = b.i32() & 7, d.armour = b.i32() & 0x3f;
                        g.damageParts.push_back(d);
                    } else if (k == 0x87) {
                        g.damageTexture = b.str();
                    } else {
                        break;
                    }
                }
                break;
            }
            default: return !g.geometry.empty();
        }
    }
    return !g.geometry.empty();
}

bool parseEcy(const Bytes& data, EcyScript& e) {
    Reader r(data);
    if (!r.ok(16)) return false;
    r.u32();
    r.u32();
    e.nsets = r.i32();
    e.xbName = r.str();
    r.u32();
    r.u32();
    r.u32();
    uint32_t ns = r.u32();
    for (uint32_t s = 0; s < ns && r.ok(12); s++) {
        EcyState st;
        st.id = r.i32();
        st.type = r.i32();
        uint32_t n = r.u32();
        for (uint32_t k = 0; k < n && r.ok(4); k++) {
            EcyTransition t;
            t.target = r.i32();
            t.tag = r.str();
            t.kind = r.i32();
            t.trigger = r.i32();
            t.d0 = r.f64();
            t.d1 = r.f64();
            t.rate = r.f32();
            uint32_t ne = r.u32();
            for (uint32_t q = 0; q < ne && r.ok(12); q++) t.events.push_back({r.i32(), r.f32(), r.f32()});
            st.transitions.push_back(std::move(t));
        }
        e.states.push_back(std::move(st));
    }
    return r.p == r.end;
}

}  // namespace hg2
