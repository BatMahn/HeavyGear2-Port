#include "weapon.h"

#include <cmath>

namespace hg2 {

namespace {
Reader openBlock(Reader& r, bool brace) {
    r.u32();  // crc
    const uint8_t* start = r.p;
    uint32_t size = r.u32();
    Reader b(start, size);
    if (start + size > r.end) b.end = r.end;
    b.p = start + 4;
    if (brace) b.u32();
    r.p = start + size;
    return b;
}
}  // namespace

bool loadWeapons(const Bytes& data, std::vector<WeaponDef>& out) {
    Reader r(data);
    if (!r.ok(12)) return false;
    r.u32();  // crc
    r.u32();  // size
    uint32_t n = r.u32();
    std::vector<uint32_t> offs;
    for (uint32_t i = 0; i < n && r.ok(4); i++) offs.push_back(r.u32() + 4);  // relative to the size field
    for (uint32_t i = 0; i < offs.size(); i++) {
        size_t end = i + 1 < offs.size() ? offs[i + 1] : data.size();
        if (offs[i] >= data.size() || end > data.size()) break;
        Reader e(data.data() + offs[i], end - offs[i]);
        WeaponDef w;
        w.classId = e.i32();
        for (uint32_t t = e.u32(); t != 0 && e.ok(); t = e.u32()) {
            if (t == 0x6d) {  // Header
                Reader b = openBlock(e, true);
                for (uint32_t k = b.u32(); k != 1 && b.ok(); k = b.u32()) {
                    switch (k) {
                        case 0x72: w.pof = b.str(); break;
                        case 0x73: w.fireSound = b.str(); break;
                        case 0x74: w.bulletSound = b.str(); break;
                        case 0x75: w.emptySound = b.str(); break;
                        case 0x76: w.damagedSound = b.str(); break;
                        case 0x7f: w.name = b.str(); break;
                        case 0x77: w.nextRoundTime = b.f32(); break;
                        case 0x78: w.velocity = std::fabs(b.f32()); break;
                        case 0x79: w.minRange = b.f32(); break;
                        case 0x7a: w.maxRange = b.f32(); break;
                        case 0x7c: w.nextBurst = b.f32(); break;
                        case 0xad: w.damage = b.f32(); break;
                        case 0x13c: w.collateralRadius = b.f32(); break;
                        case 0x152: w.mortarAngle = b.f32(); break;
                        case 0x7b: w.burst = b.i32(); break;
                        case 0x7d: w.ammunition = b.i32(); break;
                        case 0x7e: w.reticle = b.i32(); break;
                        case 0x15c: w.trailEffect = b.i32(); break;
                        case 0x15d: w.flashEffect = b.i32(); break;
                        case 0x16d: w.autoBurst = b.i32(); break;
                        case 0x80: w.type = 1; break;
                        case 0x81: w.type = 2; break;
                        case 0x82: w.type = 3; break;
                        case 0x84: w.type = 0x84; break;
                        case 0x159: w.type = 7; break;
                        default: k = 1, b.p = b.end; break;
                    }
                    if (b.p >= b.end) break;
                }
            } else if (t == 0x6f) {  // Alternate_Geometry
                Reader b = openBlock(e, true);
                for (uint32_t k = b.u32(); k != 1 && b.ok(); k = b.u32()) w.geometry.push_back(b.str());
            } else if (t == 0x6e) {  // Geometry_File
                Reader b = openBlock(e, false);
                w.geometry.push_back(b.str());
            } else if (t == 0x71) {
                w.bulletGeometry = e.str();
            } else {
                break;
            }
        }
        out.push_back(std::move(w));
    }
    return !out.empty();
}

const char* weaponForHardpointKeyword(int k) {
    switch (k) {
        case 0xb3: return "LAC";    // LightAutoCanon
        case 0xb4: return "LRP16";  // MissileLauncher
        case 0xb5: return "LMG";    // LightMachineGun
        case 0xb6: return "HMG";    // HeavyMachineGun
        case 0xb7: return "LLC";    // LaserCanon
        case 0xb8: return "SLC";    // SniperLaserCanon
        case 0xb9: return "HLC";    // HeavyLaserCanon
        case 0xba: return "IRP20";  // SwarmMissileLauncher
        case 0xbb: return "ATM";    // GuidedMissileLauncher
        case 0xbc: return "LGM";    // MortarLauncher
        case 0xbd: return "HGM";    // GuidedMortarLauncher
        case 0xbe: return "HGLC";   // GatlingLaserCanon
        case 0xbf: return "LRG";    // RailGun
        case 0xc0: return "HG";     // Grenade
        case 0xc1: return "VS";     // HandHand
        case 0xc2: return "VS";     // Sword
        case 0xc3: return "VA";     // BattleAxe
        default: return nullptr;
    }
}

}  // namespace hg2
