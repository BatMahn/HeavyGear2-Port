// Weapon definitions: MstrWpns.bin (FUN_1008da70) entries parsed like FUN_100ee440.
#pragma once
#include <string>
#include <vector>

#include "../core/fs.h"

namespace hg2 {

struct WeaponDef {
    int classId = 0;            // leading u32 (147..231: weapon family)
    std::string name;           // Weapon_Name, e.g. "LMG"
    std::string pof;            // POF
    std::string fireSound, bulletSound, emptySound, damagedSound;
    float nextRoundTime = 0.5f; // seconds between rounds
    float velocity = 200;       // Bullet_Velocity (stored negative in the data)
    float minRange = 0, maxRange = 200;
    int burst = 1;
    float nextBurst = 0;
    int ammunition = 0;         // 0 = melee / unlimited
    int reticle = 0;
    int type = 0;               // 1 two-handed, 2 melee, 3 grenade, 7 remote grenade, 0x84 haywire
    float damage = 0, collateralRadius = 0, mortarAngle = 0;
    int trailEffect = -1, flashEffect = -1, autoBurst = 0;
    std::vector<std::string> geometry;  // weapon model LODs
    std::string bulletGeometry;
    bool beam() const { return velocity >= 900; }  // lasers / particle accelerators
};

bool loadWeapons(const Bytes& data, std::vector<WeaponDef>& out);
// Weapon family name prefix for a gear hardpoint keyword (0xb3..0xc3).
const char* weaponForHardpointKeyword(int keyword);

}  // namespace hg2
