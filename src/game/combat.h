// Weapons in play: player loadout, projectiles, hits, damage and impact effects.
#pragma once
#include <algorithm>
#include <map>
#include <string>
#include <vector>

#include "../assets/gear.h"
#include "../assets/weapon.h"
#include "scene.h"

namespace hg2 {
bool isUnitObjectType(int type);  // gears, vehicles, turrets, infantry

class Player;
class Audio;
class Particles;

struct Loadout {
    const WeaponDef* def = nullptr;
    int slot = 0;      // hardpoint
    int ammo = 0;      // -1 unlimited
    float cooldown = 0;
};

class Combat {
public:
    bool init(const FileSystem& fs, Scene& scene, const GearDef& playerDef, int playerObject);
    void addUnit(int object, const GearDef& def);  // hit points from the definition (armour x10 or structure points)
    // Loadout from a shell gear variant: (hardpoint, MstrWpns.bin index) pairs replace the definition's weapons.
    void setLoadout(int object, const std::vector<std::pair<int, int>>& weapons);
    // A shell variant's construction stats (the .VAR record): armour and fire control replace the chassis' own.
    void setStats(int object, int armor, int fireControl);
    // Construction ratings and perks / flaws (the gear definition's Stats block, or the shell's variant record).
    void setSystems(int object, int sensors, int maneuver, uint32_t perks, uint32_t flaws);
    // What the systems do in a fight (see docs/FORMATS.md "Perks and flaws in the simulation"):
    int ecm(int object) const { return field(object, 0, 10, 3); }       // jams enemy fire and sensors
    int eccm(int object) const { return field(object, 0, 13, 3); }      // cancels ECM, keeps missile locks
    int stealth(int object) const { return field(object, 1, 10, 3); }   // harder to detect
    int jumpJets(int object) const { return field(object, 0, 24, 2); }
    int sensorRating(int object) const { auto it = units_.find(object); return it == units_.end() ? 0 : it->second.sensors; }
    int maneuverRating(int object) const { auto it = units_.find(object); return it == units_.end() ? 0 : it->second.maneuverStat; }
    // Scatter multiplier for `shooter` firing at `target`: the target's manoeuvre and ECM against the shooter's
    // ECCM and fire-control flaws.
    float aimSpread(int shooter, int target) const;
    // Target Designator: the designating side's shooters are steadier against that target.
    void setDesignated(int target, int team) { designated_ = target, designatorTeam_ = team; }
    void setSpace(bool on) { space_ = on; }
    bool zeroG(int object) const { return field(object, 1, 14, 1) != 0; }
    // Squad radio: lost with the sensor head unless there's a Backup Communication System.
    bool commsOk(int object) const { return sensorsOkRaw(object) || field(object, 0, 2, 1) != 0; }
    bool sensorsOkRaw(int object) const;
    bool ejection(int object) const { return field(object, 0, 9, 1) != 0; }
    bool amphibious(int object) const { return field(object, 0, 1, 1) != 0; }
    // Speed factor for wading: below the water surface gears slow down unless Amphibious.
    float wading(int object, float waterLevel) const;
    bool sniper(int object) const { return field(object, 0, 29, 1) != 0; }
    bool designator(int object) const { return field(object, 1, 13, 1) != 0; }
    bool uplink(int object) const { return field(object, 0, 31, 1) != 0; }
    // Detection range multiplier for `viewer` seeing `target`: sensors against stealth.
    float detectFactor(int viewer, int target) const;
    // Posture code of a unit (0 prone, 1 standing, 2 kneeling, 3 skating) for its concealment.
    void setPosture(int object, int code) { if (units_.count(object)) units_[object].posture = code; }
    // Concealment 0..1 (the gear's +0xc2c, FUN_10064d20): 0.15 standing / moving, 0.25 kneeling, 0.4 prone, plus 0.05
    // per Stealth level, minus 0.15 for a Large Sensors Profile; halved for a couple of seconds after firing.
    float concealment(int object) const;
    // Debrief statistics per shooter: rounds fired, hits on units, units destroyed.
    std::map<int, int> shotsBy, hitsBy, killsBy;
    void selectWeapon(int index);
    void nextWeapon();
    // Fire the shooter's selected weapon from `muzzle` toward `aim` (world point). False if not ready / empty.
    bool fire(const Vec3& muzzle, const Vec3& aim) { return fire(playerObject_, muzzle, aim); }
    bool fire(int shooter, const Vec3& muzzle, const Vec3& aim);
    // The unit `shooter` is aiming at; guided missiles (AAM, AGM, ATM, HATM) home on it.
    void setAimTarget(int shooter, int target) { aimTarget_[shooter] = target; }
    bool fireAs(int shooter, const Vec3& muzzle, const Vec3& aim) { return fire(shooter, muzzle, aim); }
    float range(int shooter) const;
    // AI: switch to the best usable weapon for a target at `dist` (melee up close, otherwise the hardest hitter
    // that reaches); returns its range, 0 if nothing usable.
    float chooseWeapon(int shooter, float dist);
    float longestRange(int shooter) const;  // of the usable weapons
    bool hasMelee(int shooter) const;
    float health(int object) const;  // 0..1, 1 for objects without hit points
    bool alive(int object) const { return health(object) > 0; }
    void setInvincible(int object, bool on) { if (units_.count(object)) units_[object].invincible = on; }
    // Network play: another machine owns this unit's armour; hits here only show (it takes damage over there).
    void setRemote(int object, bool on) { if (units_.count(object)) units_[object].remote = on; }
    // Network play: set a remote unit's state from its owner (alive and armour fraction).
    void setRemoteHealth(int object, bool alive, float fraction);
    // Back to full armour after a respawn (network play).
    void revive(int object, const GearDef& def);
    int lastAttacker(int object) const { auto it = units_.find(object); return it == units_.end() ? -1 : it->second.lastAttacker; }
    void setHitpoints(int object, float hp) {
        auto it = units_.find(object);
        if (it != units_.end()) it->second.hp = std::min(hp, it->second.maxHp > 0 ? it->second.maxHp : hp);
    }
    void update(float dt);
    void draw();
    std::string status() const;
    const std::vector<Loadout>& loadout() const { return units_.at(playerObject_).loadout; }

    // Damage taken by mission objects (index -> damage) and destruction callbacks.
    std::vector<float> damage;
    std::vector<int> destroyed;  // objects destroyed since last drain
    struct Hit { int victim, shooter, level; };
    std::vector<Hit> hits;       // hits since last drain
    std::vector<int> damaged;    // units that dropped below half their hit points
    void kill(int object);       // InstantDeath
    void grow(size_t objects) { if (damage.size() < objects) damage.resize(objects, 0.f); }
    // Per-part damage (gears with a Damage_Hierarchy): health 0..1 of a region (H head, T torso, A/a right/left
    // arm, L/l right/left leg; 1 if the unit has no such parts), movement left after leg damage, sensors working.
    float regionHealth(int object, char region) const;
    float mobility(int object) const;
    float manoeuvre(int object) const;     // turn-rate multiplier after critical hits
    bool weaponBroken(int object, int index) const;
    std::vector<int> hitLevel;             // per drained hit: 2 light / none, 3 medium, 4 heavy (engine events)
    bool sensorsOk(int object) const;
    // Muzzle of the selected weapon: its hardpoint frame (frm-hpN) at the current pose. False if unknown.
    bool muzzle(int shooter, Vec3& out) const;
    std::vector<std::string> log;  // part-destroyed messages for the HUD (drained by the game)
    Audio* audio = nullptr;
    Particles* particles = nullptr;  // effects for hits, explosions and muzzle flashes when set

private:
    struct Shot {
        Vec3 pos, vel;
        float life;
        const WeaponDef* def;
        bool beam;
        int shooter;
        int target = -1;     // guided missiles home on it while the lock holds
        bool jammed = false;
    };
    std::map<int, int> aimTarget_;
    int designated_ = -1, designatorTeam_ = -1;
    bool space_ = false;
    float clock_ = 0;  // shooter -> the unit it's firing at (for guided missiles)
    struct Part {                 // Damage_Hierarchy entry with its share of the unit's hit points
        std::string frame;        // "BDA", "RAB"...
        int system = 0;           // 1 limb, 2 structure, 3 sensors / cockpit
        float hp = 0, maxHp = 0;
        char region = 'T';        // H head, T torso, A/a right/left arm, L/l right/left leg
        bool gone = false;
    };
    struct Unit {
        std::vector<Loadout> loadout;
        int selected = 0;
        float hp = 0, maxHp = 0;
        bool invincible = false;
        bool remote = false;
        int sensors = 0, maneuverStat = 0, posture = 1;
        float firedAt = -100;
        uint32_t perks = 0, flaws = 0;
        int lastAttacker = -1;
        std::vector<Part> parts;
        float smokeTimer = 0;
        float surfaceTimer = 1.f;
        // original damage model (FUN_10058fd0 / FUN_10059240): hp holds the armour value x10, which hits wear down
        bool armoured = false;      // gear / vehicle with an armour stat; otherwise structure points
        int fireControl = 0;        // damage multiplier index for its own shots
        int mobility = 4;           // critical hits take mobility (speed) and manoeuvre (turning) levels
        int manoeuvre = 0;
        std::vector<bool> broken;   // per loadout entry: knocked out by a critical hit
        std::string damageTexture;
    };
    std::map<int, Unit> units_;
    int field(int object, int word, int shift, int width) const {
        auto it = units_.find(object);
        if (it == units_.end()) return 0;
        uint32_t w = word ? it->second.flaws : it->second.perks;
        return int(w >> shift & ((1u << width) - 1));
    }
    static void dropLevel(uint32_t& w, int shift, int width) {  // a critical hit knocks a system down a level
        uint32_t m = ((1u << width) - 1) << shift;
        if (w & m) w = (w & ~m) | ((((w & m) >> shift) - 1) << shift);
    }
    struct Flash { Vec3 pos; float age, size; };
    void impact(const Vec3& p, const WeaponDef& w, int object, int shooter, float scale = 1.f);
    void applyDamage(int object, int shooter, float d, const Vec3& p);
    void destroyObject(int object, const Vec3& p);
    std::map<int, float> structure_;  // destructible objects without an armour stat: structure points left
    std::vector<Loadout> buildLoadout(const GearDef& def);
    void partDestroyed(int object, Unit& u, Part& part, const Vec3& at);

    Scene* scene_ = nullptr;
    int playerObject_ = -1;
    std::vector<WeaponDef> weapons_;
    std::vector<Shot> shots_;
    std::vector<Flash> flashes_;
    GpuMesh *tracer_ = nullptr, *beam_ = nullptr, *flash_ = nullptr;
};

}  // namespace hg2
