// Gear / vehicle definition blocks (FUN_1004bcf0) and .ECY animation state machines.
#pragma once
#include <string>
#include <vector>

#include "../core/fs.h"

namespace hg2 {

struct GearDef {
    int targetingId = 0;
    int hitPoints = -1;  // 0xad: structure points (buildings 1000, vehicles 90, markers 1); -1 if absent
    float maxRadius = 0;
    std::vector<std::string> geometry;  // Alg LODs (Alternate_Geometry) or Geometry_Files
    std::string animScript, scriptFile, detailMap;
    // Stats (0x8f)
    int sensors = 0, fireControl = 0, armor = 0, size = 0, maneuverability = 0, movement1 = 0, movement2 = 0;
    uint32_t perks = 0, flaws = 0;  // 0x9b PerksFlaws bit fields (the shell's layout, see GameData::perkFields)
    float maxWalkSpeed = 0, maxRunSpeed = 0;
    float detection[6] = {};  // IR/UV, Radar, Heat, Ultrasonic, Motion, Magnetic
    struct Hardpoint { int slot; int weapon; int ammo; };  // weapon = keyword 0xb3..0xc3
    std::vector<Hardpoint> hardpoints;
    std::string soundMotor, soundSteps, soundDestruction, soundDamage, soundWheels;
    // Damage_Hierarchy (0x85): model frames that take damage, with a 3-bit system class and a 6-bit armour value
    // (packed into the frame's flags by FUN_1004bcf0), plus the damaged-texture name (0x87).
    struct DamagePart { std::string frame; int system = 0, armour = 0; };
    std::vector<DamagePart> damageParts;
    std::string damageTexture;
};
bool parseGearDef(const uint8_t* p, size_t n, GearDef& out);

// .ECY: per-state transitions to other states (FUN_10030960, binary path).
struct EcyTransition {
    int target;      // state index (>= state count: special/damage animations)
    std::string tag; // frame/part tag for damage transitions ("BDB")
    int kind;        // 0 normal, 1, 2, 3 damage
    int trigger;     // input / command id that selects this transition
    double d0, d1;
    float rate;      // animation keys per second
    struct Ev { int id; float a, b; };
    std::vector<Ev> events;
};
struct EcyState {
    int id = 0, type = 0;  // type 1 states loop
    std::vector<EcyTransition> transitions;
};
struct EcyScript {
    std::string xbName;
    int nsets = 0;
    std::vector<EcyState> states;
};
bool parseEcy(const Bytes& data, EcyScript& out);

}  // namespace hg2
