// Unit AI: gears, vehicles and turrets with the original's brain (FUN_100b9ae0: perception, target selection
// FUN_100b9300, engagement FUN_100bddd0, the non-combat / combat / post mode machine), the team alert levels with their
// behaviour records (FUN_100bb970), the tactic table (FUN_100dadc0 / FUN_100dd910) whose Think methods post orders
// (FUN_100d9620) to the unit's order executors (FUN_10039f40), and the gunner (FUN_1002ef10 -> FUN_10036f10).
// See docs/FORMATS.md "Unit brain".
#pragma once
#include <functional>
#include <array>
#include <map>
#include <random>
#include <string>
#include <vector>

#include "combat.h"
#include "player.h"

namespace hg2 {

class Audio;
class Particles;

// Script event slots raised on a unit's scripts by the engine (see docs/SCRIPTS.md).
enum UnitEvent {
    EV_DAMAGED = 2,          // light hit (3 medium, 4 heavy) — FUN_10058fd0
    EV_DESTROYED = 5,
    EV_HIT = 11,             // took a hit
    EV_HIT_BY_USER = 12,     // while the user is shooting it
    EV_PATH_END = 13,
    EV_ENEMY_IN = 15, EV_ENEMY_OUT = 17,
    EV_FRIEND_IN = 19, EV_FRIEND_OUT = 21,
    EV_COMBAT_START = 29, EV_COMBAT_END = 30,
};

class AiSystem {
public:
    void init(const FileSystem& fs, Scene& scene, Combat& combat, const Player& player);
    void update(float dt);
    int count() const { return int(units_.size()); }
    void setAudio(Audio* a) {
        audioHook_ = a;
        for (auto& u : units_) u.body.audio = a, u.body.soundVolume = 0.8f;
    }
    void setSpace(bool on) { space_ = on; for (auto& u : units_) u.body.space = on; }
    // Cinematic mode without AllowCinematicCombat: brains don't pick or engage targets (FUN_100b9ae0, +0x521).
    void setCombatHold(bool on) { combatHold_ = on; }
    void setParticles(Particles* p) {
        particlesHook_ = p;
        for (auto& u : units_) u.body.particles = p;
    }
    // a mission object (or one spawned later) becomes a simulated unit; teams below 100 (neutral props, the user's
    // side in single player) only when `neutral` is set (a script gave it a movement order)
    bool addUnit(int objectIndex, bool neutral = false);
    int aliveIn(const std::vector<int>& objects) const;
    int alive() const;
    void dump() const;  // debug: one line per unit
    std::function<void(int object, int event)> onEvent;

    // Script orders
    void sleep(int object, bool on);
    void gotoPoint(int object, const Vec3& p);
    bool followPath(int object, const std::string& path);
    bool assignPath(int object, const std::string& path);  // AssignPath: the path a PathFollow tactic walks
    void stop(int object);
    void speed(int object, float kph);
    void attack(int object, int target);
    void notifyRange(int object, float r);
    void command(int object, const std::string& cmd, float a, float b, float c, const std::string& s, float d = 0);
    void hitBy(int object, int shooter);  // remember the attacker, wake up
    // An enemy round in the unit's 32 m collision cell this frame (FUN_100553f0, gear vtable +0xc4, called by the
    // collision sweep FUN_100a11b0 for every round sharing the cell; FUN_100b8220 on the brain).
    void nearMiss(int object, int shooter);
    // Team +0x235 (FUN_100b88f0): whether the team's AI units cycle their sensors active / passive (FUN_100ba740)
    // A unit on a walkable graph (brain +0x1b4 the graph, +0x1b8 the node it is at or was sent to)
    void setGraph(int object, int graph, int node) { if (Unit* u = find(object)) u->graph = graph, u->graphNode = node; }
    // ORDER_MOVETONODE (kind 3, FUN_10037b40; FUN_1005c110 posts it with flags 0x200): steer straight at the node, done
    // within 2 m (DAT_10140b90), the node then becomes the brain's +0x1b8
    void moveToNode(int object, const Vec3& at, int node);
    void setAutoSensors(int team, bool on) { if (team >= 0 && team < 16) autoSensors_[size_t(team)] = on; }
    void hitReaction(int object, int level) { if (Unit* u = find(object)) u->body.hitReaction(level); }
    // Squad mates: keep a formation slot (leader-relative: x right, z behind) and fight what comes close.
    // join `leader`'s squad as member `slot` (0..3) of formation group `group` (0 = A, 1 = B)
    void follow(int object, int leader, int group, int slot);
    // The squad formation (FUN_100be4f0 on the tables of FUN_100be020 and the user squad's preset FUN_100d2e90 #4,
    // separation 64): where member `slot` of group `group` stands for a leader at `at` with matrix rows axis0 / axis2
    static Vec3 formationPoint(int group, int slot, const Vec3& at, const Vec3& axis0, const Vec3& axis2);
    static void yawAxes(float yaw, Vec3& axis0, Vec3& axis2);  // rows 0 and 2 of a gear's matrix for a heading
    int mode(int object) const { const Unit* u = find(object); return u ? u->mode : 0; }  // brain +0x44
    // back into the fight from scratch (a pooled fountain unit, an instant-action respawn): the unit is rebuilt
    // from its mission object, which the caller has moved to the new spot
    bool respawn(int object);
    // The user's squad commands (the original's key bindings, FUN_100d1bc0): attack / defend the user's target,
    // hold ground, regroup, fire at will, go to the selected nav point.
    enum SquadOrder { OrderFollow = 0, OrderAttack, OrderDefend, OrderHold, OrderFireAtWill, OrderGoto, OrderRegroup };
    void squadOrder(int leader, int order, int object, const Vec3& pos);
    int squadSize(int leader) const;
    void setAutoSleep(int object, bool on) { if (Unit* u = find(object)) u->autoSleep = on, u->dozing = u->dozing && on; }
    bool dozing(int object) const { const Unit* u = find(object); return u && u->dozing; }
    bool teleport(int object, const Vec3& p);
    bool face(int object, const Vec3& p);
    int relation(int object, int ref) const;
    // Team alert levels (scripts number teams from 0: mission team - 99)
    void setDefcon(int team, int level, int mode);
    int defcon(int team) const;

    bool position(int object, Vec3& out) const { return positionOf(object, out); }
    template <class F> void forEach(F f) const {
        for (auto& u : units_) f(u.object, u.team, u.body.position(), combat_->alive(u.object));
    }

private:
    // An order in a unit's queue (FUN_100be7f0 record: +0xc kind, +0x10 state, +0x18 timer, +0x28 flags, +0x30 point /
    // object). Kinds (FUN_100bdb90): 0 NOP (wait `param` s), 1 STAND, 2 MOVETO, 4 MOVE_DIRECT, 6 TURNTO, 10 FIREWPN_V.
    // MOVETO flags (FUN_100373c0): 1 goal is the point, 2 goal is the object `obj`, 8 hold the trigger while moving
    // in combat mode, 0x10 the move ends after `param` s, 0x20 drive backwards, 0x100 flip to backwards while the
    // target is more than 135 degrees off the nose, 0x80 the pathfinder's wading flag.
    struct Order {
        int kind = 0, flags = 0, obj = -1, node = -1;
        float param = 0;
        Vec3 pos;
        bool started = false;
        float best = 1e30f, patience = 5.f;  // MOVETO progress watchdog (FUN_100bf300)
        bool rerouted = false, reverse = false, revLatch = false;
    };
    struct Unit {
        Player body;
        int object = -1, team = 0, type = 0;
        bool mobile = true;
        bool asleep = false;
        int forced = -1;            // Attack() / squad Attack target, kept (brain +0xa4) until it dies
        std::vector<Vec3> path;     // FollowPath / goto waypoints
        size_t pathIdx = 0;
        std::vector<Vec3> route;    // the last path given: PathFollow1/2 keep walking it
        int routeIdx = -1;
        bool hop = false, assigned = false;
        float freeRadius = 0;       // ActivateFreeRange radius (+0x1ac)
        bool freeRange = false;
        float guardBest = 1e30f;    // SquadMateGuard: nearest it has been to the point
        Vec3 leaderLast;            // SquadMateFollow: where the leader was last frame
        bool following = false;
        std::vector<Vec3> detour;   // route round an obstacle to detourGoal (planRoute)
        size_t detourIdx = 0;
        Vec3 detourGoal, lastSteer;
        float detourCheck = 0;
        bool wantRoute = false;
        bool pathPaused = false;
        float speedSet = -1;        // SetSpeed throttle (brain +0x1d4, FUN_100d8790); < 0: the mode's throttle
        // brain (gear +0xd8c)
        float visRange = 250.f;     // SetVisibilityRange (+0x54, default 250 m)
        float enemyRange = 180.f;   // SetEnemyNotifyRange (+0x74, default 180 m)
        float friendRange = 64.f;   // SetFriendlyNotifyRange (+0x60, default 64 m)
        int target = -1;            // +0x84
        float targetHold = 0;       // +0x88: seconds before the target is chosen again
        int nearEnemy = -1, nearFriend = -1;  // +0x70 / +0x5c
        float nearEnemyDist = 1e30f, nearFriendDist = 1e30f;  // +0x78 / +0x64
        float seen = 0;             // +0x3c: how long an enemy has been in view
        int attacker = -1;          // +0x1e0
        float hitAgo = 1e30f;       // +0x1e4: seconds since it was hit
        bool heardShot = false;     // +0xca: hit this frame (the damage handlers set the hit level there)
        int graph = -1, graphNode = -1;  // +0x1b4 / +0x1b8: the walkable graph (its object) and node
        bool onGraph = false;       // +0x1b2: sent along a graph
        int nearShooter = -1;       // +0x1d8: whoever fired the last round that passed close (or at the squad leader)
        float nearAgo = 1e30f;      // +0x1dc: seconds since; dropped past the record's +0x1c
        bool exposed = false;       // +0xa5: the target shows up on the sensors
        bool los = false;           // +0xa0 bit 2: line of sight to the target
        bool inRange = false;       // +0xa9: within the selected weapon's range
        bool facing = false;        // +0xa0 bit 4: the target within 75 degrees of the nose
        bool targetFacing = false;  // +0xa0 bit 5: the unit within 75 degrees of the target's nose
        float targetDist = 0;       // +0x90
        float losCheck = 0;
        Vec3 lastKnown;             // +0x20: where the target was last seen
        int mode = 0;               // +0x44: 0 non-combat, 1 combat, 2 post-combat
        float combatTimer = 0;      // +0x4c
        float postTimer = 0;        // +0x50
        bool holdFire = false;      // +0xe6: never engage (squad Hold Fire)
        bool noSeenEngage = false;  // +0xe7: don't engage just for having seen an enemy a while
        bool enemyIn = false, friendIn = false, inCombat = false;
        float clock = 0;
        Vec3 lastTargetPos;
        int lastTarget = -1;
        // the tactic running (FUN_100ddbd0 run record: +8 step, +0xc timer, +0x18 counter) and its order queue
        std::string tactic;
        int tacMode = -1;
        int step = 0, count = 0;
        float tacTimer = 0;
        // the space tactics' hull graph walk (FUN_100afe70): the graph (brain +0x1b4) and node (+0x1b8) the unit is
        // on, and the tactic's run time (+0xc of its record)
        int hullGraph = -1, hullNode = -1;
        float graphClock = 0;
        Vec3 point, home, goalPos;
        int lookAt = -1;
        std::vector<Order> orders;
        bool canTarget = true;
        int drive = 0;              // Forward / Backward
        bool braked = false;        // FullBrake
        bool autoSleep = true;      // doze when out of view and over 300 m from the user (FUN_10049a40)
        bool dozing = false;
        float dozeCheck = 0;
        int leader = -1;            // squad formation
        int order = 0, orderObj = -1;  // squad command (SquadOrder) and its object
        Vec3 orderPos;
        bool scriptStance = false;  // the script set posture / skates
        int fgroup = 0, fslot = 0;  // squad formation group (A / B) and member index in it
        Vec3 slotPos;               // brain +0x110: the formation point the squad last gave it
        bool haveSlot = false;
        std::string combatTactic, nonCombatTactic;
        bool cinematic = false;     // SetCinematicControl(unit): script-driven, no targets of its own (brain +0x198)
        bool autoAim = true;        // AutoAim (brain +0xe8): the gunner tracks its target
        bool trigger = false;       // the trigger is held this frame (gear +0xbc9)
        bool resting = false;       // +0xe5: the burst is over, the gunner rests +0xe0 seconds
        float restTimer = 0;        // +0xe0
        int burstLimit = -1, burstFired = 0;  // +0xdc (Auto_Burst, at most the rounds left) / weapon +0x1e8
        bool arc = false;           // SetWeaponConstraints on a turret (FUN_100ec400): yaw limits about its placement
        float arcMin = 0, arcMax = 0, arcHome = 0;  // radians, positive = to the right of the placement
        // turret gun elevation limits (pitch joint JXA): +-30 degrees from the turret's setup (FUN_100eb0c0) until
        // SetWeaponConstraints; positive = up
        float pitchMin = -0.5235988f, pitchMax = 0.5235988f, gunPitch = 0;
        // AutoScan (brain +0xe9, on by default; off while a target is set, back on when it is dropped or on
        // AutoPilot): an idle gear sweeps its torso (FUN_10037e30)
        bool autoScan = true;
        float sensorClock = 0;      // +0x38: time in the current active / passive sensor mode
        int lastTarget2 = -1;
        float scanGoal = 0, scanPause = 0;
    };
    // Team alert (the team object FUN_100b8330): defcon level (+0x244, starts at 1, limits +0x12 / +0x14 = 1 / 5),
    // alert points (+0x18) and the time left at this level (+0x1c).
    struct Team {
        int defcon = 1, minDefcon = 1, maxDefcon = 5;
        float points = 0, timer = 1e30f;
    };
    void setLevel(int team, int level);
    void alertEvent(int team, int event);  // FUN_100b8900
    int unitClass(const Unit& u) const;    // tactic table class (FUN_100da380)
    bool tacticApplies(const Unit& u, const std::string& t, int mode) const;
    std::string pickTactic(Unit& u, int mode);
    bool think(Unit& u, float dt);           // the running tactic's Think; false = finished
    void post(Unit& u, int kind, int flags, float param, const Vec3& pos = Vec3(), int obj = -1);
    float modeThrottle(const Unit& u) const;  // FUN_100b7d00: the team record's throttle for the brain mode
    void setThrottle(const Unit& u, PlayerInput& in, float throttle) const;
    std::map<std::string, int> tacticUses_;
    std::map<int, bool> untargetable_;        // CanTarget(unit, 0): brain +0x1a clear, others skip it
    bool targetable(int object) const { auto it = untargetable_.find(object); return it == untargetable_.end() || !it->second; }   // FUN_100dd910's per-tactic use counts (+0x14)
    void scan(Unit& u, float dt);
    // a squad's formation frame (+0x1100: the leader's pose when last re-formed) and its 1 s re-assignment clock (+0x490)
    struct SquadFrame { Vec3 pos, axis0, axis2; float timer = 0; bool set = false; };
    std::map<int, SquadFrame> squads_;
    bool leaderPose(int leader, Vec3& pos, Vec3& axis0, Vec3& axis2, float& speed, float& vmax) const;
    void formation(float dt);   // FUN_100d04c0 per squad
    void assignSlots(int leader, const SquadFrame& f);  // FUN_100cd370
    Unit* find(int object);
    const Unit* find(int object) const;
    bool positionOf(int object, Vec3& out) const;
    int teamOf(int object) const;  // mission team (99 neutral, 100 user's side, 101…)
    void event(int object, int ev) {
        if (onEvent) onEvent(object, ev);
    }

    Scene* scene_ = nullptr;
    Combat* combat_ = nullptr;
    const Player* player_ = nullptr;
    std::vector<Unit> units_;
    std::mt19937 rng_{77};
    // grid A* round steep ground, solid objects and lava; false when no route within the search budget
    bool planRoute(const Vec3& from, const Vec3& to, int self, std::vector<Vec3>& out) const;
    bool lineBlocked(const Vec3& from, const Vec3& to, int self) const;
    // the MOVETO request a unit's class makes (slope cosine, water-depth limit, water penalty, nodes on the surface)
    struct MoveRules {
        float slopeCos = 0.866f, depthLimit = 1e9f;
        float radius = 1.f;        // the request's +0x98 / search +0xb8: the unit's bounding radius (object test)
        bool waterCost = true, onSurface = false;
        bool avoidDamage = true;   // request +0x1a (search +0x3a): damaging surfaces are blocked (cleared by flag 0x10)
    };
    MoveRules moveRules(int self) const;
    bool walkable(float x, float z, const MoveRules& r, int self) const;
    bool nodeWalkable(Vec3& p, const MoveRules& r, int self, int& graph) const;
    float routeBudget_ = 0;
    bool forcedTac_ = false;
    bool combatHold_ = false;
    bool space_ = false;
    const FileSystem* fs_ = nullptr;
    Audio* audioHook_ = nullptr;
    Particles* particlesHook_ = nullptr;
    Team teams_[16];
    float viewDist_ = 0;
    std::array<bool, 16> autoSensors_ = make_true();  // team +0x235 (FUN_100b88f0; on by default)
    static std::array<bool, 16> make_true() { std::array<bool, 16> a; a.fill(true); return a; }
};

}  // namespace hg2
