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
    int burstCount = 0;  // rounds of the current burst (weapon +0x1b4)
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
    void setSpace(bool on) { space_ = on; }
    bool zeroG(int object) const { return field(object, 1, 14, 1) != 0; }
    // Squad radio: lost with the sensor head unless there's a Backup Communication System.
    bool commsOk(int object) const {
        auto it = units_.find(object);
        if (it != units_.end() && it->second.commsLost) return false;
        return sensorsOkRaw(object) || field(object, 0, 2, 1) != 0;
    }
    bool sensorsOkRaw(int object) const;
    // FUN_10062980: the pilot's chance to get out when the gear is finished (+0xd88): -1 (never) unless the gear
    // was added to the user's squad (FUN_100d1e20 sets 0.8; the user's own gear is the leader, not a member).
    // The Ejection System perk itself is never tested by hg2sim.dll.
    void setEjectChance(int object, float c) { if (units_.count(object)) units_[object].ejectChance = c; }
    bool ejected(int object) const { auto it = units_.find(object); return it != units_.end() && it->second.ejected; }
    // Crippled (trigger 15) waiting for the unit's body; true once.
    bool takeCripple(int object) {
        auto it = units_.find(object);
        if (it == units_.end() || !it->second.cripple) return false;
        it->second.cripple = false;
        return true;
    }
    // FUN_1003cc30 on ground contact: a gear landing faster than 20 m/s down (+0x1080, gears: FUN_10035460) takes
    // damage word D = 2000 x (v - 20) / 20 (all of it from 40 m/s, +0x1088), no direction, critical mask 0xff.
    void hardLanding(int object, float downSpeed);
    // Ramming (FUN_100a63d0, the end of the narrow test): two units whose proxies are both dynamic, with a part of one
    // pushed through the other this frame. Each takes D = round(0.1 x ((v_a + v_b) / 22 x the other's box volume -
    // its own)) when positive, v the forward speeds (vtable +0x164), in the word D << 14 | 0x400 with mask 6.
    void setSpeed(int object, float v) { speeds_[object] = v; }
    void ram(int a, int b);
    bool amphibious(int object) const { return field(object, 0, 1, 1) != 0; }
    bool sniper(int object) const { return field(object, 0, 29, 1) != 0; }
    bool designator(int object) const { return field(object, 1, 13, 1) != 0; }
    // ---- sensors (the gear's SENSORS suite FUN_10118d80, swept every 0.2 s by the radar FUN_101117f0; see
    // docs/FORMATS.md "Sensors, ECM and detection")
    // Whether `target` shows up for `viewer`'s sensors right now (FUN_100ba1c0): its concealment (halved while its
    // sensors are active) within its sensor signal (+0xc50).
    bool exposed(int target) const;
    float signal(int object) const { auto it = units_.find(object); return it == units_.end() ? 0.f : it->second.sens.signal; }
    // Raise the unit's sensor signal by `add` through its filter (FUN_100b8220 +0.25 near shot, FUN_100b82a0 +0.85 hit).
    void reveal(int object, float add);
    // Contact detected by `team`'s sensors (object word +0x14 bits 12..: radar blips, the user's targeting).
    bool detectedBy(int object, int team) const;
    // Active / passive sensors (object flag bit 20, "Active Radar Config"): active sensors reach 4x as far but
    // halve the unit's concealment. Gears start active (FUN_100670f0).
    bool activeSensors(int object) const { auto it = units_.find(object); return it != units_.end() && it->second.sens.active; }
    void setActiveSensors(int object, bool on) { auto it = units_.find(object); if (it != units_.end()) it->second.sens.active = on; }
    // ECM (FUN_10110d00 / FUN_10110e60): the user toggles it; it runs off a 30 s charge that refills at half rate.
    void toggleEcm(int object);
    bool ecmOn(int object) const { auto it = units_.find(object); return it != units_.end() && it->second.sens.ecmOn; }
    float ecmCharge(int object) const { auto it = units_.find(object); return it == units_.end() ? 0.f : it->second.sens.ecmCharge; }
    // The unit's radar is jammed by an ECM field this sweep (FUN_101117f0 +0x775, FUN_10055ff0); guided missiles
    // lose their guidance and the user's target is dropped.
    bool radarJammed(int object) const { auto it = units_.find(object); return it != units_.end() && it->second.sens.jammed; }
    bool sensorsDamaged(int object) const { auto it = units_.find(object); return it != units_.end() && it->second.sens.damaged; }
    // Contact radius (radar +0x730): 300 m for the user; an AI unit's is its alert record's sensor reach
    // (+0x00, clamped 50..400, FUN_100b89e0).
    void setSensorReach(int object, float r) { auto it = units_.find(object); if (it != units_.end()) it->second.sens.reach = r; }
    void setViewDistance(float d) { viewDist_ = d; }
    // The heading the unit's sensors look along (vtable +0x214: a gear's head frame, so it follows the torso twist).
    // AI fire control (FUN_100b95d0 +0xa6): the share of its fired damage that hit (+0x9c = +0x98 / +0x94)
    // against 0.3 x its accuracy (+0xb90 = clamp((FC + 5) / 8, 0.05, 1)).
    bool overAccurate(int object) const;
    // Posture code of a unit (0 prone, 1 standing, 2 kneeling, 3 skating) for its concealment.
    void setPosture(int object, int code) { if (units_.count(object)) units_[object].posture = code; }
    // Concealment 0..1 (the gear's +0xc2c, FUN_10064d20): 0.15 standing / moving, 0.25 kneeling, 0.4 prone, plus 0.05
    // per Stealth level, minus 0.15 for a Large Sensors Profile; the sensors read it halved while the unit's own
    // sensors are active (flag bit 20).
    float concealment(int object) const;
    // Debrief statistics per shooter: rounds fired, hits on units, units destroyed.
    std::map<int, int> shotsBy, hitsBy, killsBy;
    void selectWeapon(int index);
    // test hook (HG2_WEAPON): arm the player's selected slot with the named MstrWpns weapon, unlimited ammo
    bool testWeapon(const std::string& name);
    void nextWeapon();
    // Fire the shooter's selected weapon from `muzzle` toward `aim` (world point). False if not ready / empty.
    bool fire(const Vec3& muzzle, const Vec3& aim) { return fire(playerObject_, muzzle, aim); }
    bool fire(int shooter, const Vec3& muzzle, const Vec3& aim);
    // The unit `shooter` is aiming at; guided missiles (AAM, AGM, ATM, HATM) home on it.
    void setAimTarget(int shooter, int target) { aimTarget_[shooter] = target; }
    bool fireAs(int shooter, const Vec3& muzzle, const Vec3& aim) { return fire(shooter, muzzle, aim); }
    float range(int shooter) const;
    int autoBurst(int shooter) const;  // Auto_Burst (keyword 0x16d, weapon +0x1b8)
    // AI target choice (FUN_100502d0): the summed damage of its loaded weapons and its armour points (gear +0xbec)
    float firepower(int object) const;
    float hitpoints(int object) const;
    float muzzleVelocity(int shooter) const;
    int ammoLeft(int shooter) const;  // of the selected weapon, -1 unlimited  // Bullet_Velocity of the selected weapon (0 for melee)
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
    // The original's frame clock (timer 0x101d3a88, FUN_1002a150 / FUN_1002a260(60) / FUN_1002a190): every pass of
    // the main loop busy-waits until at least 1/60 s has gone (the rate is clamped to 10..260 and set to 60), and a
    // frame's dt is held to at most 1 s (+0x28; 1/15 s in cinematic mode, FUN_10092f70). Everything the simulation
    // does "once a frame" therefore runs at up to 60 Hz; the port takes those steps at this rate.
    static constexpr float kFrameHz = 60.f;
    void update(float dt);
    void draw();
    std::string status() const;
    const std::vector<Loadout>& loadout() const { return units_.at(playerObject_).loadout; }

    // Damage taken by mission objects (index -> damage) and destruction callbacks.
    std::vector<float> damage;
    std::vector<int> destroyed;  // objects destroyed since last drain
    struct Hit { int victim, shooter, level; };
    std::vector<Hit> hits;       // hits since last drain
    // Rounds sharing a unit's 32 m collision cell this frame (the collision sweep FUN_100a11b0 calls every object
    // of the cell's lists with vtable +0xc4 for a round, before any box test): drained by the game into the AI.
    struct Near { int victim, shooter; };
    std::vector<Near> nearShots;
    std::vector<int> damaged;    // units that dropped below half their hit points
    void kill(int object);       // InstantDeath
    void destroy(int object);    // Kill: destroyed as if shot (explosion, wreck)
    // one ArtilleryStrike shell (FUN_10034a50): released at rest at p and falling under 20 m/s^2 until it hits
    void artilleryShell(const Vec3& p);
    // SetSmoking (FUN_100d3a40 / FUN_100d3a80): the unit's smoke level (+0xbe0); 0 puts it out, otherwise
    // FUN_100561e0 attaches DAMAGEL (level 1, and anything out of range) or DAMAGEH (2) at 1.5x scale
    void setSmoking(int object, int level) { smoking_[object] = level; }
    void grow(size_t objects) { if (damage.size() < objects) damage.resize(objects, 0.f); }
    // Per-part damage (gears with a Damage_Hierarchy): health 0..1 of a region (H head, T torso, A/a right/left
    // arm, L/l right/left leg; 1 if the unit has no such parts), movement left after leg damage, sensors working.
    float regionHealth(int object, char region) const;
    float mobility(int object) const;      // Movement left after criticals / the original Movement (vmax scales with it)
    int maneuverNow(int object) const;     // Maneuverability after criticals (+0xbf4); 0x7fff for non-units
    // Knock-downs from criticals (0x80000, FUN_10062430: ECY trigger 0x13 or 0x14) waiting for the unit's body.
    int takeKnock(int object) { auto it = units_.find(object); if (it == units_.end()) return 0; int k = it->second.knock; it->second.knock = 0; return k; }
    bool weaponBroken(int object, int index) const;
    std::vector<int> hitLevel;             // per drained hit: 2 light / none, 3 medium, 4 heavy (engine events)
    bool sensorsOk(int object) const;  // the radar isn't knocked out (FUN_10119820 case 7)
    // Muzzle of the selected weapon: its hardpoint frame (frm-hpN) at the current pose. False if unknown.
    bool muzzle(int shooter, Vec3& out) const;
    std::vector<int> ejectedList;  // gears whose pilot got out (drained by the game with `destroyed`)
    size_t droppedWeapons() const { return dropped_.size(); }
    // test runs: apply critical effect bits directly (FUN_10062430 words, e.g. 0x40000 Movement 1)
    void testCritical(int object, uint32_t bits) {
        auto it = units_.find(object);
        bool kill = false;
        if (it != units_.end()) applyCritical(object, it->second, bits, kill);
    }
    void testHit(int object, float d, const Vec3& p) { applyDamage(object, -1, d, p, 0xff); }
    void testClearFlaws(int object, uint32_t bits) { if (units_.count(object)) units_[object].flaws &= ~bits; }
    std::vector<std::string> log;  // part-destroyed messages for the HUD (drained by the game)
    bool multiplayer = false;  // DAT_101fcf44 + 0x1b: heavy hits / criticals don't knock gears down in network play
    Audio* audio = nullptr;
    Particles* particles = nullptr;  // effects for hits, explosions and muzzle flashes when set

private:
    void updateSensors(float dt);
    Vec3 sensorAxis(int object) const;  // the visual sensor's view axis (see updateSensors)
    static constexpr float kSensorZ = -1.f;  // row 2 points to the back: the test is (viewer - unit) . row 2 > 0.707
    struct Shot {
        Vec3 pos, vel;
        float life;
        const WeaponDef* def;
        bool beam;
        int shooter;
        int target = -1;     // guided missiles home on it while the shooter's radar isn't jammed (FUN_100e62b0)
        int trail = 0;       // attached trail effect (Particles handle), 0 if none
    };
    std::map<int, int> aimTarget_;
    std::map<int, float> speeds_;  // forward speed of each simulated body this frame (vtable +0x164)
    bool space_ = false;
    float viewDist_ = 1000.f;
    float decayClock_ = 0;      // DAT_1026c87c: the contact memory's one-second clock (FUN_1010fd40)
    struct Field { Vec3 pos; float radius; };
    std::vector<Field> jamFields_;  // DAT_101fcf40: ECM fields on this frame
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
        struct Sensors {
            bool active = true;       // flag bit 20
            double signal = 0, signal1 = 0;  // +0xc50 (filter +0xc38: y = 0.25 x + 0.75 y)
            uint8_t memory[8] = {};   // +0x14 low nibbles: per team, refreshed to 15 when sensed
            uint8_t seenBy = 0;       // +0x14 bits 12..: per team, detected
            float sweep = 0;          // radar +0x738 against the 0.2 s period +0x734
            uint8_t off = 0;          // sub-sensors knocked out (FUN_10119820): 1 visual 2 ultrasonic 4 motion 8 magnetic 16 radar
            bool damaged = false;     // the SENSORS display blinks (+0x179)
            bool jammed = false;      // radar +0x775
            bool ecmOn = false;       // ECM device +0x178
            float ecmCharge = 30.f;   // +0x1e8
            Vec3 lastPos;
            bool havePos = false;
            float speed = 0;
            float reach = 300.f;
            float det[6] = {};        // the definition's Detection (+0xc14..: IR/UV, radar, heat, ultrasonic, motion, magnetic)
        } sens;
        float shotDamage = 0, hitDamage = 0;  // brain +0x94 / +0x98
        uint32_t perks = 0, flaws = 0;
        int lastAttacker = -1;
        std::vector<Part> parts;
        float smokeTimer = 0;
        // original damage model (FUN_10058fd0 / FUN_10059240): hp holds the armour value x10, which hits wear down
        bool armoured = false;      // gear / vehicle with an armour stat; otherwise structure points
        int fireControl = 0;        // damage multiplier index for its own shots (+0xbe8, criticals lower it)
        bool gear = false;          // bipedal gear (vtable 0x101434a0): FUN_100618a0 / FUN_10062430 criticals
        int movement = 0, movementBase = 0;  // +0xbf8 (criticals take it down to 1)
        int maneuverNow = 0;        // +0xbf4
        int knock = 0;              // pending knock-down trigger
        bool commsLost = false;     // FUN_100622b0
        std::vector<bool> broken;   // per loadout entry: knocked out by a critical hit
        const WeaponDef* first = nullptr;  // the weapon object in slot 0 of +0xb88 (first hardpoint placed)
        std::string damageTexture;
        bool cripple = false;       // trigger 15 for the body (+0xc12 set)
        bool finishLater = false;   // this hit's knock-down critical calls FUN_10062980(1)
        float ejectChance = -1.f;   // +0xd88
        bool ejected = false;       // +0xc11: the pilot is out, the gear stands down
        float dying = -1.f;         // +0xc10 with the +0x13dc timer: destroyed when it runs out
        float drown = 0;            // +0x1328 (FUN_1003db20): time under water, +0.1 a frame
        bool underWater = false;    // +0x1324
        float vacuum = 0;           // +0x1330: the same in space without Zero-G Movement
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
    // FUN_100618a0 (gears) / FUN_10058fd0 (other units): the critical-effect bits for a hit of damage d with the
    // weapon's system mask (bullets and blasts 0xe, haywire 0x35); FUN_10062430 / FUN_10059240 apply them.
    uint32_t criticalRoll(int object, Unit& u, float severity, uint32_t mask);
    void applyCritical(int object, Unit& u, uint32_t bits, bool& kill);
    struct Flash { Vec3 pos; float age, size; };
    // FUN_100632e0: a weapon left lying where an ejected pilot's gear stood (a copy of its slot-0 weapon object,
    // the gear's own transform, the gear's rounds for it; +0x300 = 40 s, run down only in network play)
    struct Dropped { const WeaponDef* def; Mat4 world; int ammo; float life; };
    std::vector<Dropped> dropped_;
    struct Shell { Vec3 pos, vel; int trail = 0; };         // ArtilleryStrike shells in flight
    struct Blast { int object; float wait, damage; Vec3 at; int shooter = -1; uint32_t mask = 0xe; bool haywire = false; float share = 0; };  // explosion damage on its way (dist / 60 s)
    std::vector<Shell> shells_;
    std::map<int, int> smoking_;          // script-set smoke levels (override the damage smoke)
    std::map<int, float> smokeTimers_;
    std::vector<Blast> blasts_;
    // area explosion (FUN_100f3140 / FUN_100f36e0 / FUN_100f3430): every unit or structure within `radius`
    // takes the full `damage`, delayed by its distance / 60
    void blast(const Vec3& p, float radius, float damage, int object);
    void updateShells(float dt);
    void impact(const Vec3& p, const WeaponDef& w, int object, int shooter, float scale = 1.f, bool expired = false);
    // effect tables (FUN_100e4ca0): a projectile's ammo class byte (the weapon's Trail_Effect) picks its trail,
    // trail colour and ground / object explosions; bullets and beams make sparks by the ground's dust class
    static bool explosive(const WeaponDef& w);
    int startTrail(int ammoClass, const Vec3& pos);
    void explosionEffect(int ammoClass, const Vec3& p, bool onObject);
    void bulletEffect(const Vec3& p, bool onObject);
    // `directional` false: damage words without a hit direction (surfaces, drowning, landings: no facing armour)
    void applyDamage(int object, int shooter, float d, const Vec3& p, uint32_t mask = 0xe, bool haywire = false,
                     bool directional = true);
    // FUN_10062980: the gear is finished (`delayed`: the knock-down critical's call with 1, else 0)
    void finishGear(int object, Unit& u, const Vec3& p, bool delayed, bool& kill);
    // FUN_1003db20's per-frame environment damage (lava, drowning, vacuum), run at the port's 30 Hz frame
    void environment(int object, Unit& u);
    float envClock_ = 0;
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
