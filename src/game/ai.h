// Unit AI: gears, vehicles and turrets with the brain settings the mission scripts drive (notify ranges,
// visibility, focus, team alert level / defcon, combat and non-combat tactics, posture, paths), and the
// brain events the scripts listen for (FUN_100b9fe0). Target selection and fighting are still a simplified
// stand-in for the original tactic code.
#pragma once
#include <functional>
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
    // Pilot difficulty (the campaign pilot's setting, 0 easy .. 1 hard): how far hostiles see and how well they shoot.
    void setGravity(float g) { for (auto& u : units_) u.body.gravityScale = g; }
    void setDifficulty(float d) { difficulty_ = d < 0 ? 0.f : d > 1 ? 1.f : d; }
    // Cinematic mode without AllowCinematicCombat: brains don't pick or engage targets (FUN_100b9ae0, +0x521).
    void setCombatHold(bool on) { combatHold_ = on; }
    void setParticles(Particles* p) {
        particlesHook_ = p;
        for (auto& u : units_) u.body.particles = p;
    }
    bool addUnit(int objectIndex);  // a mission object (or one spawned later) becomes a simulated unit
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
    void command(int object, const std::string& cmd, float a, float b, float c, const std::string& s);
    void hitBy(int object, int shooter);  // remember the attacker, wake up
    void hitReaction(int object, int level) { if (Unit* u = find(object)) u->body.hitReaction(level); }
    // Squad mates: keep a formation slot (leader-relative: x right, z behind) and fight what comes close.
    void follow(int object, int leader, const Vec3& slot);
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
    struct Unit {
        Player body;
        int object = -1, team = 0, type = 0;
        bool mobile = true, awake = false;
        float think = 0, detect = 250;
        int target = -1;  // object index
        bool asleep = false;
        int forced = -1;            // Attack() target
        std::vector<Vec3> path;     // FollowPath / AssignPath waypoints
        size_t pathIdx = 0;
        std::vector<Vec3> route;    // the last path given: PathFollow1/2 keep walking it
        int routeIdx = -1, routeDir = 1;
        bool hop = false, assigned = false;
        float freeRadius = 0;       // ActivateFreeRange radius (+0x1ac)
        bool freeRange = false;
        float guardBest = 1e30f;    // SquadMateGuard: nearest it has been to the point           // the current path is one PathFollow hop (no path-end event)
        Vec3 leaderLast;            // SquadMateFollow: where the leader was last frame
        bool following = false;
        std::vector<Vec3> detour;   // route round an obstacle to detourGoal (planRoute)
        size_t detourIdx = 0;
        Vec3 detourGoal, lastSteer;
        float detourCheck = 0;
        bool wantRoute = false;
        bool pathPaused = false;
        float speedFrac = 1.f;      // SetSpeed relative to maximum
        // brain settings
        float enemyRange = 0, friendRange = 0;
        int enemyNear = -1, friendNear = -1, attacker = -1, lookAt = -1;
        bool enemyIn = false, friendIn = false, inCombat = false;
        float calm = 0;             // seconds without a target while in combat
        float clock = 0;
        Vec3 lastTargetPos;
        // the original's tactic layer (hg2sim FUN_100dadc0 table, Think methods FUN_100a84d0 ...): a combat tactic
        // runs a small state machine that posts orders; the orders drive the body; a gunner fires on its own
        struct Order { int kind = 0; Vec3 pos; float speed = 0, timer = 0, best = 1e30f, patience = 5.f; bool rerouted = false; };  // kind: 0 none, 1 stand, 2 move, 6 turn, 10 volley, 99 wait
        Order ord;
        std::string tactic;         // the combat tactic running ("" = none chosen yet)
        int step = 0, count = 0;
        float replan = 0, tacTimer = 0;
        Vec3 point, goal, home;
        bool visible = false, engaged = false;       // target in sight (terrain line)
        int lastTarget = -1;
        bool canTarget = true;
        int drive = 0;              // Forward / Backward
        bool braked = false;        // FullBrake
        bool autoSleep = true;      // doze when out of view and over 300 m from the user (FUN_10049a40)
        bool dozing = false;
        float dozeCheck = 0;
        int leader = -1;            // squad formation
        float blocked = 0;          // seconds the target has been out of the line of fire
        int order = 0, orderObj = -1;  // squad command (SquadOrder) and its object
        Vec3 orderPos;
        float disengage = 0;        // Regroup: no fighting for a few seconds
        bool scriptStance = false;  // the script set posture / skates: leave them alone
        bool autoSkates = false, autoKneel = false;
        float stuckClock = 0, unstick = 0;  // no progress while driving: back off and turn
        Vec3 stuckFrom;
        Vec3 slot;
        std::string combatTactic, nonCombatTactic;
        bool cinematic = false;     // SetCinematicControl(unit): script-driven, no targets of its own (brain +0x198)
        bool autoAim = true;        // AutoAim (brain +0xe8): the gunner tracks and shoots its target
        bool arc = false;           // SetWeaponConstraints on a turret (FUN_100ec400): yaw limits about its placement
        float arcMin = 0, arcMax = 0, arcHome = 0;
    };
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
        bool waterCost = true, onSurface = false;
    };
    MoveRules moveRules(int self) const;
    bool walkable(float x, float z, const MoveRules& r) const;
    float routeBudget_ = 0;
    bool forcedTac_ = false;
    bool combatHold_ = false;
    const FileSystem* fs_ = nullptr;
    Audio* audioHook_ = nullptr;
    Particles* particlesHook_ = nullptr;
    int defcon_[16], minDefcon_[16], maxDefcon_[16];
    float viewDist_ = 0;
    float difficulty_ = 0.5f;
    float alert_[16];
    void raiseAlert(int team, float points);
};

}  // namespace hg2
