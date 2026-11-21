#include "ai.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <queue>
#include <cstdio>
#include <cstring>

namespace hg2 {
namespace { inline Vec3 flat(const Vec3& v) { return Vec3(v.x, 0, v.z); } }

namespace {
constexpr float kPi = 3.14159265358979f;
float wrap(float a) {
    while (a > kPi) a -= 2 * kPi;
    while (a < -kPi) a += 2 * kPi;
    return a;
}
std::string lowerCopy(std::string s) {
    for (auto& c : s) c = char(tolower((unsigned char)c));
    return s;
}
bool has(const std::string& s, const char* k) { return s.find(k) != std::string::npos; }
float rand01() { return float(rand() % 32768) * (1.f / 32767.f); }  // rand() * DAT_101472c8 (1/32767)

// The team behaviour records, one per alert level (FUN_100bb970 fills them at +0x248 + level * 0x44 of the team
// configuration; FUN_100b89e0 points the team and every brain's +0x14 at the current one). Fields used:
// +0x00 sensor reach (FUN_100b8fc0, clamped 50..400), +0x0c engage range, +0x10 seconds in view before engaging,
// +0x18 the gunner's rest between bursts (FUN_100b7cd0: +- 0.5 s at random), +0x1c how long a hit (or a hit on a friend) keeps a unit engaged, +0x28 post-combat time, +0x2c/+0x30/+0x34 throttle
// in the non-combat / combat / post modes (FUN_100b7d00), +0x38 seconds before the level drops, +0x3c alert points
// that raise it, +0x40 the engage test (FUN_100bddd0 at every level).
struct Rec {
    float sight, engage, seenTime, react, hitMemory, postTime, throttle[3], decay, escalate;
};
constexpr Rec kRec[6] = {
    {400, 48, 0.f, 0.1f, 8, 10, {1.5f, 1.5f, 1.5f}, 1e30f, 1e30f},
    {100, 64, 8.f, 3.f, 5, 10, {0.3f, 0.85f, 0.3f}, 1e30f, 100},
    {150, 80, 5.f, 2.7f, 5, 10, {0.4f, 0.85f, 0.5f}, 20, 150},
    {250, 120, 2.f, 2.f, 10, 10, {0.5f, 0.85f, 0.6f}, 40, 100},
    {300, 250, 1.f, 1.f, 10, 10, {0.6f, 0.9f, 0.8f}, 30, 100},
    {400, 300, 0.5f, 0.5f, 20, 20, {0.8f, 1.f, 0.9f}, 15, 1e30f},
};
// FUN_100b8900: alert points per event (0 a sighting, 1, 2 entering combat, 3 an enemy round close by, 4 hit,
// 5 a unit of the team destroyed, 6)
constexpr float kAlertPoints[7] = {1, 10, 25, 5, 25, 100, 50};
constexpr float kFacing = 1.30899675f;  // DAT_10148278: 75 degrees
}  // namespace

void AiSystem::init(const FileSystem& fs, Scene& scene, Combat& combat, const Player& player) {
    scene_ = &scene;
    combat_ = &combat;
    player_ = &player;
    // the team object (FUN_100b8330) starts at alert level 1 between the limits 1 and 5
    for (int t = 0; t < 16; t++) teams_[t] = Team{}, teams_[t].timer = kRec[1].decay;
    fs_ = &fs;
    for (size_t i = 0; i < scene.mission().objects.size(); i++)
        if (int(i) != player.object()) addUnit(int(i));
    SDL_Log("AI: %zu units, %zu paths", units_.size(), scene.mission().paths.size());
}

bool AiSystem::addUnit(int index, bool neutral) {
    const MissionObject& o = scene_->mission().objects[size_t(index)];
    if (o.team < 100 && !neutral) return false;
    if (find(index)) return false;
    bool turret = o.type == 0x4b;
    // gears, vehicles, turrets, and the walkers / infantry / mounts spawned by fountains
    bool unit = o.type == 0x48 || turret || o.type == 0x4e || o.type == 0x4f || o.type == 0x50 || o.type == 0x51 || o.type == 0x52;
    if (!unit) return false;
    Unit u;
    if (!u.body.initObject(*fs_, *scene_, index)) return false;
    u.object = index;
    u.team = o.team;
    u.type = o.type;
    u.mobile = !turret;
    u.home = u.body.position();
    combat_->addUnit(index, u.body.def());
    if (audioHook_) u.body.audio = audioHook_, u.body.soundVolume = 0.8f;
    if (particlesHook_) u.body.particles = particlesHook_;
    units_.push_back(std::move(u));
    return true;
}

AiSystem::Unit* AiSystem::find(int object) {
    for (auto& u : units_)
        if (u.object == object) return &u;
    return nullptr;
}
const AiSystem::Unit* AiSystem::find(int object) const {
    for (auto& u : units_)
        if (u.object == object) return &u;
    return nullptr;
}
void AiSystem::sleep(int o, bool on) {
    if (Unit* u = find(o)) u->asleep = on;
}
void AiSystem::gotoPoint(int o, const Vec3& p) {
    if (Unit* u = find(o)) u->path = {p}, u->pathIdx = 0, u->asleep = false, u->pathPaused = false, u->braked = false;
}
bool AiSystem::followPath(int o, const std::string& name) {
    const MissionPath* p = scene_->mission().path(name);
    Unit* u = find(o);
    if (!p || !u) return false;
    u->path.clear();
    u->braked = false;
    for (auto& q : p->points) u->path.push_back(Vec3(q[0], q[1], q[2]));
    u->route = u->path, u->routeIdx = -1, u->hop = false;
    u->pathIdx = 0;
    u->asleep = false;
    u->pathPaused = false;
    return true;
}
bool AiSystem::assignPath(int o, const std::string& name) {
    // AssignPath (FUN_100d5a90 → unit vtable +0x22c) only stores the path; PathFollow1/2 read it back (+0x230)
    const MissionPath* p = scene_->mission().path(name);
    Unit* u = find(o);
    if (!p || !u) return false;
    u->route.clear();
    for (auto& q : p->points) u->route.push_back(Vec3(q[0], q[1], q[2]));
    u->routeIdx = -1, u->asleep = false, u->assigned = true;
    return true;
}
void AiSystem::stop(int o) {
    if (Unit* u = find(o)) u->path.clear(), u->drive = 0, u->braked = true;  // FullBrake: hold until ordered or attacked
}
void AiSystem::speed(int o, float kph) {
    // SetSpeed (FUN_100d8790): brain +0x1d4 = kph x 0.278 / top speed, clamped to -1..1 — the throttle path orders use
    Unit* u = find(o);
    if (!u) return;
    float vmax = u->body.def().maxRunSpeed;
    u->speedSet = vmax > 1e-5f ? std::clamp(kph * 0.278f / vmax, -1.f, 1.f) : 0.f;
}
void AiSystem::attack(int o, int t) {
    // Attack (FUN_100d89f0): lock the target (vtable +0x1c4 -> FUN_10050440, brain +0xa4) and make HuntTarget the
    // non-combat tactic
    if (Unit* u = find(o)) u->forced = t, u->target = t, u->asleep = false, u->nonCombatTactic = "hunttarget";
}
void AiSystem::notifyRange(int o, float r) {
    if (Unit* u = find(o)) u->enemyRange = std::max(0.f, r);
}

void AiSystem::command(int o, const std::string& c, float a, float b, float cc, const std::string& s, float d) {
    (void)b, (void)cc;
    if (c == "canTarget") {  // CanTarget (builtin 174): brain +0x1a, whether other units may target this one
        untargetable_[o] = a == 0;
        return;
    }
    Unit* u = find(o);
    if (!u) return;
    if (c == "visibility") u->visRange = std::max(0.f, a);  // SetVisibilityRange (+0x54)
    else if (c == "friendRange") u->friendRange = std::max(0.f, a);
    else if (c == "movementMode") u->body.setSkates(int(a) == 3), u->scriptStance = true;
    else if (c == "standingMode") {
        u->scriptStance = true;
        // script codes: 0/4 prone, 1/3 standing (3 = on skates), 2 kneeling (FUN_100d7030)
        int m = int(a);
        u->body.setPosture(m == 0 || m == 4 ? 0 : m == 2 ? 2 : 1);
        u->body.setSkates(m == 3);
    } else if (c == "forward") u->drive = a != 0 ? 1 : 0;
    else if (c == "backward") u->drive = a != 0 ? -1 : 0;
    else if (c == "pausePath") u->pathPaused = a != 0;
    else if (c == "freeRange") u->freeRange = b != 0, u->freeRadius = b != 0 ? std::max(0.f, a) : u->freeRadius;  // FUN_100d80e0
    else if (c == "combatTactic") u->combatTactic = lowerCopy(s);
    else if (c == "nonCombatTactic") u->nonCombatTactic = lowerCopy(s), u->asleep = false;
    else if (c == "lookAt") u->lookAt = int(a);
    else if (c == "autoSleep") u->autoSleep = a != 0, u->dozing = u->dozing && a != 0;
    else if (c == "autopilot") {  // back to the brain's own behaviour; FUN_100d7280 also resets auto aim and cinematic control
        u->asleep = false, u->forced = -1, u->drive = 0, u->braked = false, u->orders.clear();
        u->autoAim = true, u->cinematic = false, u->autoScan = true;
    } else if (c == "fullStop") u->body.halt();       // FUN_100d44f0: +0x158(0,0), orders kept
    else if (c == "cinematicControl") u->cinematic = a != 0;  // FUN_100d66d0
    else if (c == "autoAim") u->autoAim = a != 0;      // FUN_100d7390
    else if (c == "autoScan") u->autoScan = a != 0;    // FUN_100d7470 -> FUN_100504c0: brain +0xe9
    else if (c == "weaponArc" && !u->mobile) {  // SetWeaponConstraints(a, b, cc, d) -> FUN_100ec400
        // pitch joint [-a, b] (positive = gun down): elevation -b .. a; yaw joint [-cc, d] (positive = to the left):
        // the port's yaw grows to the right, so the turn from the placement runs -d .. cc
        const float k = 0.01745329f;
        u->pitchMin = -b * k, u->pitchMax = a * k;
        u->arc = true, u->arcMin = -d * k, u->arcMax = cc * k, u->arcHome = u->body.yaw();
    }
}

// Idle sensor sweep (FUN_10037e30, per frame while +0xe9 is set, the unit has its twist joints and no target): the
// torso turns at pi/2 rad/s (+0x13b8) to a yaw goal of up to pi/2 on alternate sides (rand * pi/2 / 32767, sign
// flipped each time), with a pitch goal of up to 0.3 rad likewise (the port's gears have no torso pitch); once there it
// rests 0.5 + 3 * rand / 32767 s before picking the next goal. The joints' own limits clamp the swing.
void AiSystem::scan(Unit& u, float dt) {
    if (!u.autoScan || u.target >= 0 || u.type != 0x48 || u.cinematic || !combat_->alive(u.object)) return;
    if (u.scanPause > 1e-5f) {
        if ((u.scanPause -= dt) > 1e-5f) return;
        u.scanGoal = float(rand() % 32768) * (1.5707964f / 32767.f) * (u.scanGoal < 0 ? 1.f : -1.f);
    }
    float goal = std::clamp(u.scanGoal, -1.2f, 1.2f), cur = u.body.twist, step = 1.5707964f * dt;
    if (std::fabs(goal - cur) <= 1e-5f) u.scanPause = 0.5f + float(rand() % 32768) * (3.f / 32767.f);
    else u.body.twist = std::fabs(goal - cur) > step ? cur + (goal > cur ? step : -step) : goal;
}

void AiSystem::hitBy(int o, int shooter) {
    // FUN_100b82a0: the attacker goes to +0x1e0 with its clock +0x1e4 at 0; the team takes alert event 4
    Unit* u = find(o);
    if (!u || shooter < 0 || shooter == o) return;
    u->attacker = shooter, u->hitAgo = 0;
    u->braked = false;
    if (teamOf(shooter) != u->team) alertEvent(u->team - 99, 4);
}

bool AiSystem::teleport(int o, const Vec3& p) {
    Unit* u = find(o);
    if (!u) return false;
    u->body.teleport(Vec3(p.x, scene_->groundHeight(p.x, p.z), p.z));
    return true;
}
bool AiSystem::face(int o, const Vec3& p) {
    Unit* u = find(o);
    if (!u) return false;
    Vec3 d = p - u->body.position();
    if (d.x * d.x + d.z * d.z > 0.01f) u->body.setYaw(std::atan2(-d.x, -d.z));
    return true;
}

void AiSystem::follow(int object, int leader, const Vec3& slot) {
    if (Unit* u = find(object)) u->leader = leader, u->slot = slot, u->autoSleep = false;
}

void AiSystem::squadOrder(int leader, int order, int object, const Vec3& pos) {
    // The user's squad commands (FUN_100d1bc0). Attack / Defend give the mates the user's target as a locked target /
    // SquadMateDefend charge; Hold and Go To run SquadMateGuard on a point (Go To: the mate's formation offset round
    // the nav point, FUN_100cef60 -> FUN_100be4f0); Fire At Will clears the hold-fire flags (FUN_100cfd00(mate, 0):
    // +0xe6 / +0xe7); Regroup drops the order and goes back to SquadMateFollow.
    for (auto& u : units_) {
        if (u.leader != leader || !combat_->alive(u.object)) continue;
        u.order = order, u.orderObj = object, u.orderPos = pos;
        u.braked = false;
        u.forced = order == OrderAttack ? object : -1;
        if (order == OrderAttack) u.target = object, u.targetHold = 0;
        if (order == OrderHold) u.orderPos = u.body.position();
        if (order == OrderFireAtWill) u.holdFire = false, u.noSeenEngage = false, u.order = OrderFollow;
        if (order == OrderRegroup) u.order = OrderFollow;
        if (order == OrderGoto) {
            Vec3 lp;
            float ly = player_ && leader == player_->object() ? player_->yaw() : 0.f;
            if (const Unit* l = find(leader)) ly = l->body.yaw();
            (void)lp;
            Vec3 right(-std::cos(ly), 0, std::sin(ly)), back(std::sin(ly), 0, std::cos(ly));
            u.orderPos = pos + right * u.slot.x + back * u.slot.z;
        }
    }
}

int AiSystem::squadSize(int leader) const {
    int n = 0;
    for (auto& u : units_)
        if (u.leader == leader && combat_->alive(u.object)) n++;
    return n;
}

int AiSystem::relation(int o, int ref) const {
    if (ref >= 7 && ref <= 0x17) {  // the user's squad mates: gears on the user's side, by mission order
        int want = ref <= 0x11 ? ref - 7 : ref - 0x12, n = 0, side = teamOf(player_->object());
        for (auto& u : units_)
            if (u.team == side && u.type == 0x48 && n++ == want) return u.object;
        return -1;
    }
    const Unit* u = find(o);
    if (!u) return -1;
    if (ref == 1) return u->nearEnemy >= 0 ? u->nearEnemy : u->target;
    if (ref == 2) return u->nearFriend;
    if (ref == 5) return u->attacker >= 0 ? u->attacker : u->target;
    return -1;
}

// ---- team alert (FUN_100b89e0 / FUN_100b8900, the decay at the end of the brain pass FUN_100ba7c0)
void AiSystem::setLevel(int team, int level) {
    if (team < 0 || team >= 16) return;
    Team& T = teams_[team];
    T.defcon = std::max(T.minDefcon, std::min(T.maxDefcon, level));
    T.points = 0, T.timer = kRec[std::clamp(T.defcon, 0, 5)].decay;
}
void AiSystem::alertEvent(int team, int ev) {
    if (team < 0 || team >= 16 || ev < 0 || ev > 6) return;
    Team& T = teams_[team];
    const Rec& R = kRec[std::clamp(T.defcon, 0, 5)];
    float p = kAlertPoints[ev];
    T.points += p;
    T.timer = std::min(T.timer + 0.5f * p, R.decay);
    if (T.points > R.escalate) setLevel(team, T.defcon + 1);
}

void AiSystem::setDefcon(int team, int level, int mode) {
    if (team < 0 || team >= 16) return;
    Team& T = teams_[team];
    level = std::max(0, std::min(5, level));
    if (mode == 1) T.minDefcon = level, setLevel(team, std::max(T.defcon, level));
    else if (mode == 2) T.maxDefcon = level, setLevel(team, std::min(T.defcon, level));
    else setLevel(team, level);
}
int AiSystem::defcon(int team) const { return team >= 0 && team < 16 ? teams_[team].defcon : 6; }

void AiSystem::dump() const {
    for (auto& u : units_) {
        Vec3 p = u.body.position();
        SDL_Log("  unit %d %s team %d at %.0f,%.0f %smode %d tac %s path %zu/%zu tgt %d hp %.2f", u.object,
                scene_->mission().objects[size_t(u.object)].name.c_str(), u.team, p.x, p.z, u.asleep ? "asleep " : "",
                u.mode, u.tactic.c_str(), u.pathIdx, u.path.size(), u.target, combat_->health(u.object));
    }
}

int AiSystem::aliveIn(const std::vector<int>& objects) const {
    int n = 0;
    for (int o : objects)
        if (combat_->alive(o)) n++;
    return n;
}

int AiSystem::alive() const {
    int n = 0;
    for (auto& u : units_)
        if (combat_->alive(u.object)) n++;
    return n;
}

int AiSystem::teamOf(int object) const {
    const auto& objs = scene_->mission().objects;
    return object >= 0 && size_t(object) < objs.size() ? objs[size_t(object)].team : 0;
}

bool AiSystem::positionOf(int object, Vec3& out) const {
    if (object == player_->object()) {
        out = player_->position();
        return true;
    }
    for (auto& u : units_)
        if (u.object == object) {
            out = u.body.position();
            return true;
        }
    return false;
}

// ---- tactics (FUN_100dadc0's table, FUN_100dd910's choice)
int AiSystem::unitClass(const Unit& u) const {
    // brain +0x42 by unit class: 2 gear, 3 infantry, 5 mount, 6 chicken walker, 8 turret, 9 vehicle (jeep / tank),
    // 10 hover
    switch (u.type) {
        case 0x4b: return 8;
        case 0x4e: return 9;
        case 0x4f: return 10;
        case 0x50: return 3;
        case 0x51: return 5;
        case 0x52: return 6;
        default: return 2;
    }
}

bool AiSystem::tacticApplies(const Unit& u, const std::string& t, int mode) const {
    // each tactic's +0x20 "applies" method, with the modes it was registered for (+0x1c combat, +0x1d post,
    // +0x1e non-combat)
    bool weapon = combat_->longestRange(u.object) > 0;  // FUN_100bc870: a usable weapon
    if (t == "gearcombathand2hand")  // FUN_100aa8d0: a melee weapon, and no target 16 m or more away while a gun is usable
        return mode == 1 && combat_->hasMelee(u.object) && !(u.target >= 0 && u.targetDist >= 16.f && weapon);
    if (t == "infantrycover") return mode == 1;  // FUN_100a9500: somewhere to take cover (not modelled: always)
    if (t == "defaultrunaway")  // FUN_100ab460: hit in the last 6 s, or out of non-combat, or an enemy in view
        return (u.attacker >= 0 && u.hitAgo < 6.f) || mode != 0 || u.nearEnemy >= 0;
    if (has(t, "pursue") || t == "chaseenemy" || has(t, "patrol") || t == "defaultholdground") return true;
    return mode == 1 ? weapon : true;  // FUN_100a8410 / FUN_100a9000 / FUN_100a9830
}

std::string AiSystem::pickTactic(Unit& u, int mode) {
    // FUN_100b9510: the script's tactic for the mode if it applies, else FUN_100dd910: of the class's tactics for the
    // mode that apply, the one used least so far (weight +0x10 = 0 minus uses +0x14; the first registered wins a tie),
    // else the default list (combat DefaultRunAway, post DefaultHoldGround, non-combat DefaultPatrol)
    std::string mine = mode == 1 ? u.combatTactic : mode == 0 ? u.nonCombatTactic : std::string();
    if (!mine.empty() && mine != "default" && mine != "none" && tacticApplies(u, mine, mode)) return mine;
    static const std::vector<std::string> kGear[3] = {{"gearpatrol1"}, {"gearcombat1", "gearcombat3", "gearcombathand2hand", "gearflankright", "gearflankleft"}, {"gearpursue1"}};
    static const std::vector<std::string> kInf[3] = {{"infantrypatrol1"}, {"infantrycombat1", "infantrycover"}, {"infantrypursue1"}};
    static const std::vector<std::string> kMount[3] = {{"mountpatrol1"}, {"mountcombat1"}, {"mountpursue1"}};
    static const std::vector<std::string> kCw[3] = {{"chickenwalkerpatrol1"}, {"chickenwalkercombat1", "chickenwalkercombat2"}, {"chickenwalkerpursue1"}};
    static const std::vector<std::string> kTurret[3] = {{}, {"turretcombat1"}, {}};
    static const std::vector<std::string> kVehicle[3] = {{"vehiclepatrol1"}, {"tankattack"}, {"jeeppursue"}};
    static const std::vector<std::string> kHover[3] = {{"pa_t1_c3", "pa_t2_c3", "pa_t3_c3"}, {"hovercombat1", "hoverflankright", "hoverflankleft", "hovercircle"}, {"hoverpursue1"}};
    static const std::string kDefault[3] = {"defaultpatrol", "defaultrunaway", "defaultholdground"};
    const std::vector<std::string>* list = kGear;
    switch (unitClass(u)) {
        case 3: list = kInf; break;
        case 5: list = kMount; break;
        case 6: list = kCw; break;
        case 8: list = kTurret; break;
        case 9: list = kVehicle; break;
        case 10: list = kHover; break;
        default: break;
    }
    const std::string* best = nullptr;
    int bestScore = 0;
    for (auto& t : list[mode])
        if (tacticApplies(u, t, mode) && (!best || -tacticUses_[t] > bestScore)) best = &t, bestScore = -tacticUses_[t];
    if (best) return tacticUses_[*best]++, *best;
    if (tacticApplies(u, kDefault[mode], mode)) return tacticUses_[kDefault[mode]]++, kDefault[mode];
    return std::string();
}

void AiSystem::post(Unit& u, int kind, int flags, float param, const Vec3& pos, int obj) {
    Order o;
    o.kind = kind, o.flags = flags, o.param = param, o.pos = pos, o.obj = obj;
    u.orders.push_back(o);
}

float AiSystem::modeThrottle(const Unit& u) const {
    int lvl = std::clamp(defcon(u.team - 99), 0, 5);
    return kRec[lvl].throttle[std::clamp(u.mode, 0, 2)];
}

void AiSystem::setThrottle(const Unit& u, PlayerInput& in, float t) const {
    // The original's throttle drives the gear's force model, whose steady speed on the flat is sqrt(throttle) of the
    // top speed (FUN_1003c0b0 / FUN_1003c5f0, see "Gear ground physics"); the port's body takes a walk / run choice
    // and a fraction of that pace.
    float a = std::min(1.f, std::fabs(t)), walk = std::max(0.1f, u.body.def().maxWalkSpeed), run = std::max(walk, u.body.def().maxRunSpeed);
    float v = std::sqrt(a) * run;
    if (t < 0) in.throttle = -std::min(1.f, v / walk), in.run = false;
    else if (v > walk) in.throttle = v / run, in.run = true;
    else in.throttle = v / walk, in.run = false;
}

// The running tactic's Think (vtable slot 11), posting orders and stepping its run record; false when it is done
// (the brain then picks again, FUN_100dddd0 / FUN_100b9510).
bool AiSystem::think(Unit& u, float dt) {
    const std::string& tac = u.tactic;
    Vec3 me = u.body.position(), tp;
    bool haveTarget = u.target >= 0 && combat_->alive(u.target) && positionOf(u.target, tp);
    bool queued = !u.orders.empty();
    auto around = [&](const Vec3& c, float r, float ang) { return c + Vec3(std::sin(ang), 0, std::cos(ang)) * r; };
    auto randomWithin = [&](float r) {  // FUN_100b6db0(0, r, -pi, pi): a random offset up to r at a random bearing
        float rr = r * rand01(), ang = (rand01() * 2.f - 1.f) * kPi;
        return Vec3(std::sin(ang) * rr, 0, std::cos(ang) * rr);
    };
    // ---- combat tactics
    if (tac == "gearcombat1" || tac == "infantrycombat1" || tac == "hovercombat1" || tac == "framecombat1") {
        // FUN_100a84d0 (start FUN_100a8440: counter 4, a POSITION order with the mode's throttle)
        switch (u.step) {
            case 0: {
                if (u.count <= 0) return false;
                u.count--;
                if (haveTarget && u.los) {
                    if (!u.facing) { post(u, 6, 0, 3.f, tp, u.target); u.step = 1; return true; }  // TURNTO
                    if (u.inRange) {
                        if (combat_->longestRange(u.object) <= 0) return false;  // vtable +0x18c: nothing to fire
                        float r = rand01();
                        if (u.targetDist >= 32.f) u.step = r < 0.2f ? 4 : 6;
                        else if (!u.targetFacing) u.step = r < 0.1f ? 4 : 5;
                        else u.step = 6;
                        return true;
                    }
                    post(u, 2, 1, 3.f, tp);  // MOVETO the target until in range
                    u.step = 2;
                    return true;
                }
                // no target in sight: to where it was last seen, shooting if it shows, then stand
                post(u, 2, 9, 3.f, u.lastKnown), post(u, 1, 0, 0);
                u.step = 3;
                return true;
            }
            case 1:
                if (!queued) u.step = 0;
                return true;
            case 2:
                u.combatTimer = 2.f;  // brain +0x4c: stay in combat while closing
                if (queued && !u.inRange) return true;
                u.orders.clear(), u.step = 0;  // FUN_100534b0
                return true;
            case 3:
                if (haveTarget && u.los) u.orders.clear(), u.step = 0;
                else if (!queued) u.step = 0;
                return true;
            case 4:  // FIREWPN_V 2 s
                if (!haveTarget) { u.step = 0; return true; }
                post(u, 10, 2, 2.f, tp, u.target);
                u.step = 1;
                return true;
            case 5: {  // a 64 m move at 90 degrees round the target, backwards, firing, 4 s at most
                if (!haveTarget) { u.step = 0; return true; }
                float b = std::atan2(me.x - tp.x, me.z - tp.z);
                post(u, 2, 0xb9, 4.f, around(tp, 64.f, b + kPi * 0.5f));
                u.step = 1;
                return true;
            }
            case 6: {  // a run across the target's front: its position + its heading x 32 + a 64 m side step
                if (!haveTarget) { u.step = 0; return true; }
                Vec3 fwd(0, 0, -1);
                if (const Unit* t = find(u.target)) fwd = Vec3(-std::sin(t->body.yaw()), 0, -std::cos(t->body.yaw()));
                else if (u.target == player_->object()) fwd = Vec3(-std::sin(player_->yaw()), 0, -std::cos(player_->yaw()));
                Vec3 side(-fwd.z, 0, fwd.x);
                Vec3 p = tp + fwd * 32.f + side * (rand01() < 0.5f ? 64.f : -64.f);
                post(u, 2, 0x199, 4.f, p);
                u.step = 1;
                return true;
            }
        }
        return false;
    }
    if (tac == "gearcombat3" || tac == "hovercircle" || tac == "framecombat3") {
        // FUN_100a90e0: every 2 s check there's still a weapon (+0x18c); step 0 picks a spot and returns to the start
        // point afterwards: no target in sight / reach -> where it was last seen; 32 m or more away -> a random point
        // within 32 m; closer -> straight away from the target. Moves fire on the way (0x109).
        if ((u.tacTimer -= dt) < 0) {
            if (combat_->longestRange(u.object) <= 0) return false;
            u.tacTimer = 2.f;
        }
        switch (u.step) {
            case 0: {
                Vec3 goal = me;
                u.point = me, u.step = 2;
                if (!haveTarget) goal = me + randomWithin(32.f), u.step = 3;
                else if (!u.los || !u.inRange) goal = u.lastKnown, u.step = 3;
                else if (u.targetDist >= 32.f) goal = me + randomWithin(32.f);
                else {
                    Vec3 away = flat(me - tp);
                    float l = length(away);
                    goal = me + (l > 0.01f ? away * (32.f / l) : Vec3(32.f, 0, 0));
                    u.point = goal;
                }
                post(u, 2, 0x109, 3.f, goal);
                return true;
            }
            case 1: post(u, 2, 0x109, 3.f, u.point), u.step = 4; return true;
            case 2: if (!queued) u.step = 1; return true;
            case 3:
                if (u.los && u.inRange) u.orders.clear();
                if (u.orders.empty()) u.step = 0;
                return true;
            case 4: if (!queued) u.step = 0; return true;
        }
        return false;
    }
    if (has(tac, "flank") || tac == "frontal") {
        // FUN_100a9ee0 (angles from the constructor FUN_100a9770: GearFlankLeft / Right +-75, FlankLeft1 / Right1 +-45,
        // FlankLeft2 / Right2 +-90, Frontal 0): round to the angle off the target's bearing firing on the way, turn in,
        // FIREWPN_V 5 s
        float ang = has(tac, "left") ? 75.f : has(tac, "right") ? -75.f : 0.f;
        if (tac == "flankleft1") ang = 45.f;
        if (tac == "flankright1") ang = -45.f;
        if (tac == "flankleft2") ang = 90.f;
        if (tac == "flankright2") ang = -90.f;
        if (queued) return true;
        if (!haveTarget) return false;
        if (u.step == 0) {
            float b = std::atan2(me.x - tp.x, me.z - tp.z);
            float r = std::min(u.targetDist, std::max(32.f, combat_->range(u.object) * 0.7f));
            post(u, 2, 0x109, 3.f, around(tp, r, b + ang * 0.017453f));
            u.step = 1;
        } else if (u.step == 1) {
            post(u, 6, 9, 3.f, tp, u.target);
            u.step = 2;
        } else {
            if (u.los) post(u, 10, 10, 5.f, tp, u.target);
            u.step = 0;
            return u.los;
        }
        return true;
    }
    if (has(tac, "hand2hand")) {
        // FUN_100aacc0: MOVETO the target (flags 1), then wait 3 s (NOP)
        if (queued) return true;
        if (!haveTarget) return false;
        if (u.step == 0) post(u, 2, 1, 3.f, tp, u.target), post(u, 0, 0, 3.f), u.step = 1;
        else return false;
        return true;
    }
    if (has(tac, "turretcombat") || tac == "planecombat1") {
        // FUN_100aaef0: FIREWPN_V 5 s at the target in sight
        if (queued) return true;
        if (!haveTarget) return false;
        if (u.los) post(u, 10, 10, 5.f, tp, u.target);
        return true;
    }
    if (tac == "tankattack") {
        // FUN_100ab040: in sight 80 % FIREWPN_V 5 s, else a move firing on the way (9); out of sight, at it
        if (queued) return true;
        if (!haveTarget) return false;
        if (u.los) {
            if (rand01() > 0.8f) post(u, 2, 9, 3.f, around(me, 40.f, rand01() * 2 * kPi));
            else post(u, 10, 10, 5.f, tp, u.target);
        } else {
            post(u, 2, 9, 3.f, u.lastKnown);
        }
        return true;
    }
    if (tac == "gearcombat2" || tac == "chickenwalkercombat2" || tac == "mountcombat1") {
        // GearCombat2 / ChickenWalkerCombat2 / MountCombat1 (FUN_100a8dd0, start FUN_100a8da0: five rounds): in sight
        // and reach TURNTO + FIREWPN_V 3 s; else MOVETO where the target was last seen (flags 1) until it is in
        // sight and reach; the tactic ends when that move ends
        switch (u.step) {
            case 0:
                if (u.count <= 0) return false;
                u.count--;
                if (haveTarget && u.los && u.inRange) {
                    if (combat_->longestRange(u.object) <= 0) return false;
                    post(u, 6, 0, 3.f, tp, u.target), post(u, 10, 0, 3.f, tp, u.target);
                    u.step = 1;
                } else {
                    post(u, 2, 1, 3.f, u.lastKnown);
                    u.step = 2;
                }
                return true;
            case 1: if (!queued) u.step = 0; return true;
            case 2:
                if (!queued) return false;
                if (u.inRange && u.los) u.orders.clear(), u.step = 0;
                return true;
        }
        return false;
    }
    if (tac == "chickenwalkercombat1") {
        // FUN_100aa530: in sight and reach: 50 % a move firing on the way (9) and a TURNTO (9), else TURNTO (9) +
        // FIREWPN_V 5 s (10); otherwise MOVETO where the target was last seen firing on the way (9) until it is in
        // sight and reach. (The move's offset comes from FUN_100b6eb0, not traced: a random point within 32 m here.)
        if (u.step == 0) {
            if (haveTarget && u.los && u.inRange) {
                if (combat_->longestRange(u.object) <= 0) return false;
                if (rand01() > 0.5f) {
                    float rr = 32.f * rand01(), ang = rand01() * 2 * kPi;
                    post(u, 2, 9, 3.f, me + Vec3(std::sin(ang) * rr, 0, std::cos(ang) * rr)), post(u, 6, 9, 3.f, tp, u.target);
                } else {
                    post(u, 6, 9, 3.f, tp, u.target), post(u, 10, 10, 5.f, tp, u.target);
                }
                u.step = 1;
            } else {
                post(u, 2, 9, 3.f, u.lastKnown);
                u.step = 2;
            }
        } else if (u.step == 1) {
            if (!queued) u.step = 0;
        } else {
            if (haveTarget && u.los && u.inRange) u.orders.clear();
            if (u.orders.empty()) u.step = 0;
        }
        return true;
    }
    if (tac == "standandshoot") {
        // FUN_100a8ba0: TURNTO, FIREWPN_V 4 s
        if (queued) return true;
        if (!haveTarget) return false;
        post(u, 6, 0, 3.f, tp, u.target), post(u, 10, 0, 4.f, tp, u.target);
        return true;
    }
    if (tac == "infantrycover") {
        // FUN_100a9610: move to cover firing (0x109); the port has no cover finder and gets down where it stands
        u.body.setPosture(0);
        if (queued) return true;
        if (!haveTarget) return false;
        if (u.los) post(u, 10, 10, 2.f, tp, u.target);
        return true;
    }
    if (tac == "droneattack1") {
        // FUN_100b06e0: runs in, breaks off after a random few seconds or when close, repeats
        if (!haveTarget) return false;
        if ((u.tacTimer -= dt) <= 0) u.step = u.step == 1 ? 2 : 1, u.tacTimer = 3.f + rand01() * 4.f;
        if (u.targetDist < 25.f) u.step = 2;
        u.orders.clear();
        float b = std::atan2(me.x - tp.x, me.z - tp.z);
        post(u, 2, 9, 0, u.step == 1 ? tp : around(tp, 120.f, b));
        return true;
    }
    if (tac == "defaultrunaway") {
        // FUN_100ab550: drop the target, turn away from the threat, MOVETO 30 m away for at most 30 s (0x211)
        if (queued) return true;
        if (u.step != 0) return false;
        Vec3 from = haveTarget ? tp : u.lastKnown;
        float b = std::atan2(me.x - from.x, me.z - from.z);
        post(u, 2, 0x211, 30.f, around(me, 30.f, b));
        u.step = 1;
        return true;
    }
    if (has(tac, "pursue") || tac == "chaseenemy") {
        // FUN_100af9b0: MOVETO where the target was last seen (flags 1), STAND, wait 3 s; again while the post-combat
        // time lasts
        if (queued) return true;
        if (u.step == 1 && u.postTimer < 1e-5f) return false;
        post(u, 2, 1, 0, u.lastKnown), post(u, 1, 0, 0), post(u, 0, 0, 3.f);
        u.step = 1;
        return true;
    }
    if (tac == "defaultholdground") {
        // FUN_100afb50: TURNTO the threat, STAND, wait
        if (queued) return true;
        if (u.attacker >= 0 && positionOf(u.attacker, tp)) post(u, 6, 1, 0, tp);
        post(u, 1, 0, 0), post(u, 0, 0, 3.f);
        return true;
    }
    return false;  // non-combat tactics run in the non-combat section of update()
}

void AiSystem::update(float dt) {
    if (const char* ft = getenv("HG2_FORCETAC"); ft && !forcedTac_) {  // test hook: "unit:tactic:path"
        forcedTac_ = true;
        char tac[64] = {}, path[64] = {};
        int o = -1;
        if (sscanf(ft, "%d:%63[^:]:%63s", &o, tac, path) == 3 && find(o)) {
            if (std::string(path) == "longest")  // the mission path with the most points
                for (auto& mp : scene_->mission().paths)
                    if (const MissionPath* cur = scene_->mission().path(path); !cur || mp.points.size() > cur->points.size())
                        snprintf(path, sizeof path, "%s", mp.name.c_str());
            find(o)->nonCombatTactic = tac;
            if (strstr(tac, "pathfollow2")) find(o)->combatTactic = tac, find(o)->freeRadius = 16.f;
            SDL_Log("forcetac %d %s %s: %d mobile %d leader %d braked %d type %x", o, tac, path, assignPath(o, path), find(o)->mobile, find(o)->leader, find(o)->braked, find(o)->type);
        }
    }
    const bool tacDbg = getenv("HG2_TACDBG") != nullptr;
    routeBudget_ = getenv("HG2_NOROUTE") ? 0.f : 2.f;  // route plans per frame
    // the team alert decays (end of the brain pass, FUN_100ba7c0): points at 0.5 a second, and the level drops a step
    // when its time runs out
    for (int t = 0; t < 16; t++) {
        Team& T = teams_[t];
        T.points = std::max(0.f, T.points - 0.5f * dt);
        if (T.timer < 1e29f && (T.timer -= dt) < 0) setLevel(t, T.defcon - 1);
    }
    if (viewDist_ <= 0) {  // the far clip / fog limit bounds what the user can see
        FrameParams fp = scene_->frameDefaults();
        viewDist_ = std::max(350.f, std::min(1500.f, fp.fogEnd));
        if (scene_->mission().clipping > 0) viewDist_ = std::min(viewDist_, std::max(350.f, float(scene_->mission().clipping)));
    }
    int playerTeam = teamOf(player_->object());
    bool playerAlive = player_->active() && combat_->alive(player_->object());
    for (auto& u : units_) {
        if (!combat_->alive(u.object)) {
            if (u.type == 0x48) u.body.die(), u.body.update(dt, PlayerInput{});  // fall and lie as a wreck
            if (u.mode >= 0) alertEvent(u.team - 99, 5), u.mode = -1;  // FUN_10062980: the team loses a unit
            continue;
        }
        Vec3 me = u.body.position();
        // units out of view and over 300 m from the user are not simulated (every 2 s, FUN_10049a40)
        if (u.autoSleep && playerAlive) {
            if ((u.dozeCheck -= dt) <= 0) {
                u.dozeCheck = 2.f;
                Vec3 d = me - player_->position();
                float dist = length(Vec3(d.x, 0, d.z));
                Vec3 fwd(-std::sin(player_->yaw()), 0, -std::cos(player_->yaw()));
                bool inView = dist < viewDist_ && dot(Vec3(d.x, 0, d.z) * (1.f / std::max(1.f, dist)), fwd) > 0.35f;
                u.dozing = dist > 300.f && !inView;
            }
        } else {
            u.dozing = false;
        }
        if (u.dozing) continue;
        u.clock += dt;
        int sideIdx = u.team - 99;
        const Rec& R = kRec[std::clamp(defcon(sideIdx), 0, 5)];
        u.body.speedScale = combat_->mobility(u.object), u.body.maneuverNow = combat_->maneuverNow(u.object);
        if (int k = combat_->takeKnock(u.object)) u.body.knockDown(k);
        float sensorMul = combat_->sensorsOk(u.object) ? 1.f : 0.5f;
        // ---- perception (FUN_100ba1c0 over the sensor's contacts, FUN_100b9980, FUN_100b95d0)
        // Contacts: units within the sensor's reach (the record's +0x00) and the visibility range (+0x54) that show
        // up on the sensors. The sensor signal itself (FUN_10111xxx against the target's concealment +0xc2c, halved
        // while it fires) is the port's Combat::detectFactor approximation.
        float contactR = std::min(u.visRange, std::clamp(R.sight, 50.f, 400.f)) * sensorMul;
        auto exposedTo = [&](int obj, float d) { return d <= contactR * combat_->detectFactor(u.object, obj); };
        {
            float bestE = 1e30f, bestF = 1e30f;
            int nearE = -1, nearF = -1;
            auto look = [&](int obj, int team, const Vec3& p) {
                if (obj == u.object || !combat_->alive(obj) || dozing(obj)) return;
                float d = length(p - me);
                if (d > u.visRange) return;
                if (team == u.team) {
                    if (d < bestF) bestF = d, nearF = obj;
                } else if (team >= 100 && targetable(obj) && exposedTo(obj, d) && d < bestE) {
                    bestE = d, nearE = obj;
                }
            };
            if (playerAlive) look(player_->object(), playerTeam, player_->position());
            for (auto& o : units_) look(o.object, o.team, o.body.position());
            u.nearEnemy = nearE, u.nearEnemyDist = bestE, u.nearFriend = nearF, u.nearFriendDist = bestF;
            // notify ranges (FUN_100b9ae0 +0x68 / +0x7c bit 0, events from FUN_100b9fe0)
            bool ein = bestE <= u.enemyRange, fin = bestF <= u.friendRange;
            if (ein != u.enemyIn) event(u.object, ein ? EV_ENEMY_IN : EV_ENEMY_OUT);
            if (fin != u.friendIn) event(u.object, fin ? EV_FRIEND_IN : EV_FRIEND_OUT);
            u.enemyIn = ein, u.friendIn = fin;
        }
        // +0x3c: time an enemy has been in view, up to the record's +0x10; it drains at twice the rate
        if (u.nearEnemy >= 0) u.seen = std::min(u.seen + dt, R.seenTime);
        else u.seen = std::max(0.f, u.seen - 2.f * dt);
        if (u.attacker >= 0) u.hitAgo += dt;
        if (u.attacker >= 0 && !combat_->alive(u.attacker)) u.attacker = -1, u.hitAgo = 1e30f;
        // ---- target (FUN_100b9300): a locked target is kept; otherwise the target is held for the team's 5 s
        // (+0x23c) and chosen again: whoever hit the unit within the record's +0x1c, else the best contact by the
        // team's targeting mode 2 (FUN_100bcaf0: -distance + 5 x the firepower of its loaded weapons + its armour
        // points, FUN_100502d0), else the nearest contact; none: look again in 0.5 s
        bool cinematicMode = combatHold_ || u.cinematic;
        if (u.forced >= 0 && !combat_->alive(u.forced)) u.forced = -1;
        if (u.order == OrderAttack && u.forced < 0) u.order = OrderFollow;
        if (u.order == OrderDefend && (u.orderObj < 0 || !combat_->alive(u.orderObj))) u.order = OrderFollow;
        if (u.forced >= 0) {
            if (u.target != u.forced) positionOf(u.forced, u.lastKnown);
            u.target = u.forced;
        } else if (!(cinematicMode && u.cinematic)) {
            bool keep = u.target >= 0 && combat_->alive(u.target) && (u.targetHold -= dt) > 1e-5f;
            if (!keep) {
                int cand = -1;
                if (u.attacker >= 0 && u.hitAgo < R.hitMemory && teamOf(u.attacker) != u.team && targetable(u.attacker)) cand = u.attacker;
                if (cand < 0) {
                    float best = -1e30f;
                    auto score = [&](int obj, int team, const Vec3& p) {
                        if (obj == u.object || team == u.team || team < 100 || !combat_->alive(obj) || dozing(obj) || !targetable(obj)) return;
                        float d = length(p - me);
                        if (d > u.visRange || !exposedTo(obj, d)) return;
                        float s = -d + 5.f * combat_->firepower(obj) + combat_->hitpoints(obj);
                        if (s > best) best = s, cand = obj;
                    };
                    if (playerAlive) score(player_->object(), playerTeam, player_->position());
                    for (auto& o : units_) score(o.object, o.team, o.body.position());
                    if (cand < 0) cand = u.nearEnemy;
                }
                u.targetHold = cand >= 0 ? 5.f : 0.5f;
                if (cand != u.target && cand >= 0) positionOf(cand, u.lastKnown);
                u.target = cand;
            }
        }
        if (u.target >= 0 && !combat_->alive(u.target)) u.target = -1;
        // ---- what the brain knows of its target
        Vec3 tp;
        bool haveTarget = u.target >= 0 && positionOf(u.target, tp);
        if (haveTarget) {
            u.targetDist = length(tp - me);
            u.exposed = exposedTo(u.target, u.targetDist) || (u.target == u.attacker && u.hitAgo < R.hitMemory) || u.target == u.forced;
            if ((u.losCheck -= dt) <= 0 || u.target != u.lastTarget)
                u.losCheck = 0.25f, u.los = scene_->terrainRay(me + Vec3(0, 5, 0), tp + Vec3(0, 4, 0)) > 0.98f;
            if (getenv("HG2_LOSDBG") && int(u.clock) != int(u.clock - dt))
                SDL_Log("los %d->%d me %.1f,%.1f,%.1f (g %.1f) tp %.1f,%.1f,%.1f (g %.1f) ray %.2f", u.object, u.target, me.x, me.y, me.z,
                        scene_->groundHeight(me.x, me.z), tp.x, tp.y, tp.z, scene_->groundHeight(tp.x, tp.z), scene_->terrainRay(me + Vec3(0, 5, 0), tp + Vec3(0, 4, 0)));
            if (u.los || u.exposed) u.lastKnown = tp;
            Vec3 d = flat(tp - me);
            u.facing = std::fabs(wrap(std::atan2(-d.x, -d.z) - u.body.yaw())) < kFacing;
            float ty = u.target == player_->object() ? player_->yaw() : 0.f;
            if (const Unit* t = find(u.target)) ty = t->body.yaw();
            u.targetFacing = std::fabs(wrap(std::atan2(d.x, d.z) - ty)) < kFacing;
            float reach = combat_->chooseWeapon(u.object, u.targetDist);
            u.inRange = reach > 0 && u.targetDist <= std::max(reach, 6.f);
        } else {
            u.los = u.exposed = u.inRange = false;
        }
        // ---- engage? (the record's +0x40, FUN_100bddd0)
        bool engage = haveTarget && u.exposed && !u.holdFire &&
                      ((u.attacker >= 0 && u.hitAgo < R.hitMemory) || u.heardShot || u.nearEnemyDist <= R.engage ||
                       (!u.noSeenEngage && u.seen >= R.seenTime) || u.target == u.forced);
        u.heardShot = false;
        if (u.asleep || !u.canTarget) engage = false;
        // ---- the mode machine (FUN_100b9ae0, +0x44): combat lasts 6 s past the last moment it engages; then the
        // post-combat mode for the record's +0x28 (cut to 2 s once it would engage again); a target it doesn't engage
        // puts a non-combat unit in post mode. Entering combat is alert event 2. Cinematic mode without
        // AllowCinematicCombat holds every brain in non-combat.
        int oldMode = u.mode;
        if (cinematicMode) {
            u.mode = 0;
        } else if (u.mode == 0) {
            if (engage) u.mode = 1, alertEvent(sideIdx, 2);
            else if (haveTarget) u.mode = 2, u.postTimer = R.postTime;
        } else if (u.mode == 1) {
            if (engage) u.combatTimer = 6.f;
            if ((u.combatTimer -= dt) < 0) u.mode = 2, u.postTimer = R.postTime;
        } else if (u.mode == 2) {
            u.postTimer -= dt;
            if (engage && u.postTimer > 2.f) u.postTimer = 2.f;
            if (u.postTimer < 0) {
                if (engage) u.mode = 1, u.combatTimer = 6.f, alertEvent(sideIdx, 2);
                else u.mode = 0;
            }
        }
        bool combat = u.mode == 1;
        if (combat != u.inCombat) event(u.object, combat ? EV_COMBAT_START : EV_COMBAT_END);
        u.inCombat = combat;
        // ---- the tactic for the mode: a new mode starts a new one; a finished one is replaced (FUN_100b9510)
        if (u.mode != oldMode || u.tacMode != u.mode) {
            u.orders.clear(), u.tactic.clear(), u.tacMode = u.mode;
        }
        if (u.tactic.empty() && u.mode != 0 && !(u.mode == 2 && u.leader >= 0)) {
            u.tactic = pickTactic(u, u.mode);
            u.step = 0, u.count = has(u.tactic, "combat2") || u.tactic == "mountcombat1" ? 5 : 4, u.tacTimer = 0;
            if (tacDbg) SDL_Log("ai %d mode %d tactic %s", u.object, u.mode, u.tactic.c_str());
        }
        // a target switches the scan off (FUN_100503c0 / FUN_10050440 clear +0xe9), losing it switches it back on
        if (u.target >= 0) u.autoScan = false;
        else if (u.lastTarget2 >= 0) u.autoScan = true;
        u.lastTarget2 = u.target;
        scan(u, dt);
        if (u.asleep) {
            u.body.update(dt, PlayerInput{});
            continue;
        }
        PlayerInput in;
        auto steerTo = [&](const Vec3& p, float& err) {
            u.lastSteer = p;
            Vec3 aim = p;
            if (!u.detour.empty()) {
                if (length(flat(u.detourGoal) - flat(p)) > 12.f) u.detour.clear();
                else {
                    while (u.detourIdx < u.detour.size() && length(flat(u.detour[u.detourIdx]) - flat(me)) < 6.f) ++u.detourIdx;
                    if (u.detourIdx >= u.detour.size()) u.detour.clear();
                    else aim = u.detour[u.detourIdx];
                }
            }
            Vec3 d = aim - me;
            err = wrap(std::atan2(-d.x, -d.z) - u.body.yaw());  // models face -Z
            in.turn = std::max(-1.f, std::min(1.f, err * 2.f));
            d = p - me;
            return length(Vec3(d.x, 0, d.z));
        };
        // the throttle orders move at: SetSpeed's for paths, else the record's for the brain mode (FUN_100b7d00)
        float modeT = modeThrottle(u), pathT = u.speedSet >= 0 ? u.speedSet : modeT;
        u.trigger = false;
        Vec3 leaderPos;
        bool pathFirst = !u.path.empty() && u.pathIdx < u.path.size() && u.forced < 0 &&
                         (combat_->range(u.object) <= 0 || (u.team == playerTeam && u.type != 0x48));
        bool tacticDrives = !pathFirst && u.mode != 0 && !u.tactic.empty() && !(u.mode == 2 && u.leader >= 0);
        // PathFollow1 (FUN_100ae440): from the nearest node of the unit's path, MOVE_DIRECT to a neighbour chosen at
        // random (50/50 back or on), clamped at the ends, so the unit wanders up and down its path for good.
        // PathFollow2 (FUN_100ae910, mode 0): walk the nodes in turn, looping, each with a random offset.
        // With no non-combat tactic of its own, a unit that has an assigned path gets PathFollow1 from the tactic table
        // (FUN_100b9510 → FUN_100dd910 picks the first registered tactic that applies).
        bool ncDefault = u.nonCombatTactic.empty() || u.nonCombatTactic == "default" || u.nonCombatTactic == "none";
        if (u.mobile && !tacticDrives && u.path.empty() && u.route.size() > 1 && u.leader < 0 && !u.braked &&
            (has(u.nonCombatTactic, "pathfollow") || (ncDefault && u.assigned))) {
            int n = int(u.route.size());
            if (u.routeIdx < 0) {  // start at the nearest node
                float best = 1e30f;
                for (int i = 0; i < n; ++i)
                    if (float d = length(flat(u.route[i]) - flat(me)); d < best) best = d, u.routeIdx = i;
            } else if (has(u.nonCombatTactic, "pathfollow2")) {
                u.routeIdx = (u.routeIdx + 1) % n;
            } else {
                u.routeIdx = std::clamp(u.routeIdx + (rand01() <= 0.5f ? -1 : 1), 0, n - 1);
            }
            Vec3 p = u.route[u.routeIdx];
            if (has(u.nonCombatTactic, "pathfollow2")) p = p + Vec3(rand01() * 2 - 1, 0, rand01() * 2 - 1) * 8.f;
            u.path = {p}, u.pathIdx = 0, u.hop = true;
            if (tacDbg) SDL_Log("ai %d pathfollow node %d/%d at %.0f,%.0f", u.object, u.routeIdx, n, p.x, p.z);
        }
        if (tacticDrives) {
            // PathFollow2 in combat (FUN_100aed10): keep to the path, MOVE_DIRECT firing on the move (0x108) to the
            // node nearest the target that is still at least the free-range radius from it, preferring the
            // neighbouring nodes (×0.8, FUN_100ae820), one node at a time; once there, drift within the radius
            if (u.mode == 1 && u.mobile && u.route.size() > 1 && has(u.combatTactic, "pathfollow2") && haveTarget) {
                if (u.orders.empty()) {
                    int n = int(u.route.size()), cur = std::max(0, u.routeIdx), pick = cur;
                    float bestD = 1e30f;
                    for (int i = 0; i < n; ++i) {
                        float d = length(flat(u.route[i]) - flat(tp));
                        if (d < u.freeRadius) continue;
                        if (std::abs(i - cur) < 2) d *= 0.8f;
                        if (d < bestD) bestD = d, pick = i;
                    }
                    if (pick != cur) pick = cur + (pick > cur ? 1 : -1);
                    u.routeIdx = pick;
                    Vec3 p = u.route[pick];
                    if (pick == cur && u.freeRadius > 0) {
                        float a2 = rand01() * 2 * kPi, rr = u.freeRadius * rand01();
                        p = p + Vec3(std::sin(a2), 0, std::cos(a2)) * rr;
                    }
                    post(u, 4, 0x108, 0, p);
                }
            } else if (!think(u, dt)) {
                if (tacDbg) SDL_Log("ai %d tactic %s done", u.object, u.tactic.c_str());
                u.tactic.clear(), u.orders.clear();
            }
        }
        // ---- the order executor (FUN_10039f40)
        if (!u.orders.empty()) {
            Order& o = u.orders.front();
            bool done = false;
            Vec3 goal = o.pos, op;
            if ((o.flags & 2 || o.kind == 6 || o.kind == 10) && o.obj >= 0 && positionOf(o.obj, op)) goal = op;
            switch (o.kind) {
                case 0:  // NOP: wait
                    if ((o.param -= dt) <= 0) done = true;
                    break;
                case 1:  // STAND (FUN_10036ea0): stop
                    done = true;
                    break;
                case 2:
                case 4: {  // MOVETO / MOVE_DIRECT (FUN_100373c0)
                    if (!u.mobile) { done = true; break; }
                    if ((o.flags & 8) && u.mode == 1) u.trigger = true;
                    if ((o.flags & 0x10) && (o.param -= dt) <= 0) done = true;
                    if (o.flags & 0x100) {  // the target behind (beyond 135 degrees): drive the other way round
                        float off = haveTarget ? std::fabs(wrap(std::atan2(-(tp - me).x, -(tp - me).z) - u.body.yaw())) : 0.f;
                        if (off <= 2.3561945f) o.revLatch = false;
                        else if (!o.revLatch) o.revLatch = true, o.flags ^= 0x20;
                    }
                    bool back = (o.flags & 0x20) != 0;
                    if (!o.started) {  // the MOVETO asks the pathfinder for its waypoints when it starts (FUN_100bf690)
                        o.started = true;
                        if (length(flat(goal - me)) > 12.f && lineBlocked(me, goal, u.object)) u.wantRoute = true, u.lastSteer = goal;
                    }
                    float e2, d2 = steerTo(goal, e2);
                    if (back) {
                        e2 = wrap(e2 + kPi);
                        in.turn = std::max(-1.f, std::min(1.f, e2 * 2.f));
                    }
                    if (d2 < 6.f) { done = true; break; }
                    // FUN_100373c0: drives on once the heading is (nearly) right, else turns on the spot
                    if (std::fabs(e2) < 0.15f) setThrottle(u, in, back ? -modeT : modeT);
                    break;
                }
                case 6: {  // TURNTO (FUN_10038490)
                    if ((o.flags & 8) && u.mode == 1) u.trigger = true;
                    float e2;
                    steerTo(goal, e2);
                    if (!u.mobile || std::fabs(e2) < 0.05f) done = true;
                    break;
                }
                case 10: {  // FIREWPN_V (FUN_100371d0): stand, hold the trigger on the target for the order's time
                    if (o.obj < 0 || !combat_->alive(o.obj)) { done = true; break; }
                    u.trigger = true;
                    if ((o.param -= dt) <= 0) done = true;
                    break;
                }
                default: done = true; break;
            }
            if (done && !u.orders.empty()) u.orders.erase(u.orders.begin());
        } else if (pathFirst || u.mode == 0 || (u.mode == 2 && u.leader >= 0) || u.tactic.empty()) {
            // ---- non-combat behaviour: scripted moves, paths and the non-combat tactics
            if (u.drive != 0) {  // Forward / Backward
                in.throttle = float(u.drive);
            } else if (u.mobile && !u.path.empty() && u.pathIdx < u.path.size()) {  // follow waypoints
                if (!u.pathPaused) {
                    // advance past waypoints that are reached, passed, or farther than the one after them
                    while (u.pathIdx + 1 < u.path.size()) {
                        Vec3 a0 = flat(u.path[u.pathIdx]), a1 = flat(u.path[u.pathIdx + 1]), p = flat(me);
                        bool passed = dot(a1 - a0, p - a0) > 0.f && dot(a1 - a0, a1 - a0) > 1e-3f;
                        if (passed || length(p - a1) < length(p - a0)) u.pathIdx++;
                        else break;
                    }
                    float err, dist = steerTo(u.path[u.pathIdx], err);
                    float reach = std::max(8.f, std::fabs(u.body.speed()) * 0.8f);
                    if (dist < reach) {
                        if (++u.pathIdx >= u.path.size()) {
                            u.path.clear(), u.pathIdx = 0;
                            if (!u.hop) event(u.object, EV_PATH_END);
                            u.hop = false;
                        }
                    } else if (std::fabs(err) < 1.2f) {
                        setThrottle(u, in, pathT);
                    }
                }
            } else if (u.mobile && u.leader >= 0 && (u.order == OrderHold || u.order == OrderGoto)) {
                // SquadMateGuard (FUN_100ae0e0): MOVETO the guard point while that brings it nearer; STAND once it
                // stops gaining (blocked, or there). A new order resets the best distance.
                float err, dist = steerTo(u.orderPos, err);
                if (length(flat(u.orderPos) - flat(u.point)) > 1.f) u.point = u.orderPos, u.guardBest = 1e30f, u.step = 0;
                if ((u.tacTimer -= dt) <= 0) {  // judged every second
                    u.tacTimer = 1.f;
                    if (dist < u.guardBest - 0.5f) u.guardBest = dist, u.step = 1;
                    else u.step = 2;
                }
                if (u.step != 2 && dist > 6.f) {
                    if (std::fabs(err) < 1.2f) setThrottle(u, in, modeT);
                } else {
                    in.turn = 0;
                    if (dist > 12.f && u.step == 2 && u.tacTimer > 0.95f) u.guardBest = dist + 20.f;  // try again later
                }
            } else if (u.mobile && u.leader >= 0 && u.order == OrderDefend && positionOf(u.orderObj, tp)) {
                // SquadMateDefend (FUN_100adc20): radius r (32 m). Far (≥0.6 r): MOVETO the charge until inside 0.4 r.
                // Close and the charge standing still: wander to a random point within r of it, STAND, wait 3 s.
                const float r = 32.f;
                float dist = length(flat(tp) - flat(me));
                Vec3 tv = (tp - u.leaderLast) * (1.f / std::max(0.016f, dt));
                u.leaderLast = tp;
                if (u.step == 3 && dist < 0.4f * r) u.step = 0, u.tacTimer = 0;
                if (u.step == 0 || u.step == 4) {
                    if (dist >= 0.6f * r) u.step = 3;
                    else if (u.step == 0 && dist <= r - 10.f && length(tv) <= 0.1f) {
                        float a2 = rand01() * 2 * kPi, rr = r * rand01();
                        u.point = tp + Vec3(std::sin(a2), 0, std::cos(a2)) * rr, u.step = 4;
                    }
                }
                if (u.step == 3 || u.step == 4) {
                    Vec3 g = u.step == 3 ? tp : u.point;
                    float e2, d2 = steerTo(g, e2);
                    if (d2 < 4.f) u.step = u.step == 4 ? 5 : 0, u.tacTimer = 3.f;
                    else if (std::fabs(e2) < 1.2f) setThrottle(u, in, modeT);
                } else if (u.step == 5) {  // STAND + wait 3 s
                    float e2;
                    steerTo(tp, e2), in.turn = 0;
                    if ((u.tacTimer -= dt) <= 0) u.step = 0;
                }
            } else if (u.mobile && u.leader >= 0 && positionOf(u.leader, leaderPos) && combat_->alive(u.leader)) {
                // formation slot behind the leader, rotated by the leader's heading (models face -Z)
                float ly = u.leader == player_->object() ? player_->yaw() : u.body.yaw();
                if (const Unit* l = find(u.leader)) ly = l->body.yaw();
                Vec3 right(-std::cos(ly), 0, std::sin(ly)), back(std::sin(ly), 0, std::cos(ly));
                Vec3 want = leaderPos + right * u.slot.x + back * u.slot.z;
                // SquadMateFollow (FUN_100ad160, parameters 32 / 4 / 16 m): set off for the slot once it is more than 16 m
                // away (or 4 m with the leader on the move), settle inside 4 m, full throttle to catch up past 32 m
                float lspeed = length(flat(leaderPos) - flat(u.leaderLast)) / std::max(0.016f, dt);
                u.leaderLast = leaderPos;
                float err, dist = steerTo(want, err);
                if (!u.following && (dist > 16.f || (lspeed > 1.f && dist > 4.f))) u.following = true;
                if (u.following && dist < 4.f) u.following = false;
                if (u.following) {
                    if (std::fabs(err) < 1.2f) setThrottle(u, in, dist >= 32.f ? 1.f : modeT);
                } else {  // in place: face the way the leader faces
                    float e2 = wrap(ly - u.body.yaw());
                    in.turn = std::fabs(e2) > 0.1f ? std::max(-1.f, std::min(1.f, e2 * 2.f)) : 0.f;
                }
            } else if (u.mobile && playerAlive && teamOf(player_->object()) != u.team && has(u.nonCombatTactic, "hunt")) {
                // HuntUser (FUN_100ab880): MOVETO where the user is, re-planned only once the user has moved more
                // than 128 m from that spot; HuntTarget (FUN_100aba50) aims 16 m to a random side, re-plans past 64 m
                bool huntTarget = has(u.nonCombatTactic, "hunttarget");
                Vec3 pp = player_->position();
                if (huntTarget && u.forced >= 0) positionOf(u.forced, pp);
                float limit = huntTarget ? 64.f : 128.f;
                if (u.step != 7 || length(u.point - pp) > limit) {
                    u.point = pp;
                    u.goalPos = pp + (huntTarget ? Vec3(rand01() * 2 - 1, 0, rand01() * 2 - 1) * 16.f : Vec3());
                    u.step = 7;
                }
                float err, dist = steerTo(u.goalPos, err);
                if (dist < 6.f) u.step = 0;  // there: look again from here
                else if (std::fabs(err) < 1.2f) setThrottle(u, in, modeT);
            } else if (u.mobile && u.leader < 0 && (has(u.nonCombatTactic, "patrol") || has(u.nonCombatTactic, "freerange"))) {
                // GearPatrol1 / VehiclePatrol1 / FreeRange1 (FUN_100abee0, FUN_100af590, FUN_100af2f0): MOVETO a random
                // spot round home, STAND, wait 3 s, again
                bool vehicle = u.type == 0x4e || u.type == 0x4f;
                if (u.step == 0) {
                    float span = u.freeRadius > 0 && has(u.nonCombatTactic, "freerange") ? u.freeRadius : vehicle ? 160.f : 64.f;
                    float r = span * rand01(), a2 = rand01() * 2 * kPi;
                    u.point = u.home + Vec3(std::sin(a2), 0, std::cos(a2)) * r, u.step = 8;
                }
                if (u.step == 8) {
                    float e2, d2 = steerTo(u.point, e2);
                    if (d2 < 6.f) u.step = 9, u.tacTimer = 3.f;
                    else if (std::fabs(e2) < 1.2f) setThrottle(u, in, modeT);
                } else if (u.step == 9 && (u.tacTimer -= dt) <= 0) {
                    u.step = 0;
                }
            } else if (u.lookAt >= 0 && positionOf(u.lookAt, tp)) {
                float err;
                steerTo(tp, err);
                if (std::fabs(err) < 0.05f) in.turn = 0;
            }
        }
        // ---- the gunner (FUN_1002ef10 -> FUN_10036f10, while AutoAim): the weapons track the target's lead point —
        // its position plus its velocity over the round's flight time x 1.25 (FUN_100b6960; no lead for weapons whose
        // rounds don't fly, velocity under 1e-5) — through the torso twist; the trigger (gear +0xbc9) is held by
        // FIREWPN orders and by moves flagged to fire in combat mode, and the weapon's own cadence (Next_Round_Time,
        // Burst rounds then a NextBurst pause) decides when rounds leave. Rounds go where the gun points.
        if (haveTarget && u.autoAim && !cinematicMode) {
            Vec3 vel = (tp - u.lastTargetPos) * (1.f / std::max(0.016f, dt));
            if (u.lastTarget != u.target || length(vel) > 60.f) vel = Vec3();
            Vec3 mz;
            if (!combat_->muzzle(u.object, mz)) mz = me + Vec3(0, 5.f, 0);
            float vb = combat_->muzzleVelocity(u.object);
            Vec3 aim = tp + Vec3(0, 3.5f, 0);
            if (vb > 1e-5f) aim = aim + vel * (length(aim - mz) / vb * 1.25f);
            Vec3 d = aim - mz;
            float want = std::atan2(-d.x, -d.z);
            float gunYaw;
            if (u.mobile) {
                u.body.twist = std::clamp(wrap(want - u.body.yaw()), -1.2f, 1.2f);
                gunYaw = u.body.yaw() + u.body.twist;
            } else {  // a turret's yaw joint follows the target (the arc limits apply below)
                float e = wrap(want - u.body.yaw());
                in.turn = std::max(-1.f, std::min(1.f, e * 2.f));
                gunYaw = u.body.yaw();
            }
            float horiz = std::sqrt(d.x * d.x + d.z * d.z), elev = std::atan2(d.y, std::max(0.01f, horiz));
            if (!u.mobile) {  // the gun's pitch joint, clamped to its limits
                u.gunPitch = std::clamp(elev, u.pitchMin, u.pitchMax);
                elev = u.gunPitch;
            }
            // FUN_10050500 (gear vtable +0x180), whether the held trigger lets rounds go: never with hold fire (+0xe6);
            // after Auto_Burst rounds (at most the rounds left) the gunner rests for the record's +0x18 +- 0.5 s
            // (FUN_100b7cd0); a round goes only with a line of sight (+0xa0 bit 2) when the weapon (+0x118,
            // FUN_100efaf0: melee always; else the lead point within Max_Range and within the target's bounding radius of
            // the gun's line, FUN_100b63f0) is on and no friend or neutral nearer than the target + 5 m is in the line
            // of fire (FUN_100b7e00 -> FUN_100bab20 / FUN_100b64a0)
            if (u.trigger && !u.holdFire) {
                if (u.resting && (u.restTimer -= dt) <= 1e-5f) {
                    u.resting = false, u.burstFired = 0;
                    int ammo = combat_->ammoLeft(u.object);
                    u.burstLimit = combat_->autoBurst(u.object);
                    if (ammo >= 0) u.burstLimit = std::min(u.burstLimit, ammo);
                }
                if (u.burstLimit < 0) u.burstLimit = combat_->autoBurst(u.object);
                if (!u.resting) {
                    Vec3 gun(-std::sin(gunYaw) * std::cos(elev), std::sin(elev), -std::cos(gunYaw) * std::cos(elev));
                    Vec3 rel = aim - mz;
                    float along = dot(rel, gun), off = std::sqrt(std::max(0.f, dot(rel, rel) - along * along));
                    bool melee = combat_->muzzleVelocity(u.object) <= 0;
                    bool onTarget = melee || (along > 0 && length(rel) <= combat_->range(u.object) &&
                                              off < std::max(1.f, scene_->objectRadius(u.target)));
                    bool clear = true;
                    if (onTarget && !melee) {
                        float tt = 2.f;
                        int hit = scene_->raycast(mz, aim, u.object, tt);
                        if (hit >= 0 && hit != u.target && tt < 1.f && length(rel) * tt < u.targetDist + 5.f) {
                            int ht = teamOf(hit);
                            clear = !(ht == u.team || (ht < 100 && find(hit)));
                        }
                    }
                    if ((u.los || melee) && onTarget && clear) {
                        Vec3 shot = mz + gun * std::max(1.f, along);
                        combat_->setAimTarget(u.object, u.target);
                        if (combat_->fire(u.object, mz, shot)) {
                            u.burstFired++;
                            if (tacDbg) SDL_Log("tac %d fires at %d (%.0f m)", u.object, u.target, u.targetDist);
                        }
                    }
                    if (u.burstLimit <= u.burstFired) u.resting = true, u.restTimer = kRec[std::clamp(defcon(sideIdx), 0, 5)].react + rand01() - 0.5f;
                }
            }
        }
        if (tacDbg && int(u.clock * 2) != int((u.clock - dt) * 2) && (u.target >= 0 || u.mode != 0))
            SDL_Log("tac %d mode %d %s step %d ord %d/%zu tgt %d dist %.0f los %d exp %d rng %d trig %d defcon %d", u.object, u.mode,
                    u.tactic.c_str(), u.step, u.orders.empty() ? -1 : u.orders.front().kind, u.orders.size(), u.target,
                    u.targetDist, int(u.los), int(u.exposed), int(u.inRange), int(u.trigger), defcon(sideIdx));
        u.lastTargetPos = tp, u.lastTarget = u.target;
        // MOVETO progress watchdog (FUN_100bf300): the follower keeps the nearest it has been to its goal and a 5 s
        // timer, reset whenever it gets nearer; when the timer runs out the move fails (state 7) and the tactic picks
        // again. The port first tries a planned route, then gives the order up.
        if (!u.orders.empty() && (u.orders.front().kind == 2 || u.orders.front().kind == 4) && !u.braked && u.mobile) {
            Order& o = u.orders.front();
            Vec3 goal = o.pos, op;
            if ((o.flags & 2) && o.obj >= 0 && positionOf(o.obj, op)) goal = op;
            float d = length(flat(goal) - flat(me));
            if (d < o.best - 0.5f) o.best = d, o.patience = 5.f;
            else if ((o.patience -= dt) <= 0) {
                if (!o.rerouted) o.rerouted = true, o.patience = 5.f, u.wantRoute = true, u.lastSteer = goal;
                else u.orders.erase(u.orders.begin()), u.detour.clear();
            }
        }
        // route round what's in the way: when stuck, or (every 1.5 s) when the straight line ahead is blocked
        if (u.mobile && !u.braked && std::fabs(in.throttle) > 0.2f && u.detour.empty() && length(flat(u.lastSteer) - flat(me)) > 20.f) {
            if ((u.detourCheck -= dt) <= 0) {
                u.detourCheck = 1.5f + float(u.object % 7) * 0.05f;
                Vec3 ahead = me + (flat(u.lastSteer) - flat(me)) * std::min(1.f, 48.f / length(flat(u.lastSteer) - flat(me)));
                if (routeBudget_ > 0 && lineBlocked(me, ahead, u.object)) u.wantRoute = true;
            }
        }
        if (u.braked) u.wantRoute = false;
        if (u.wantRoute && routeBudget_ > 0) {
            u.wantRoute = false, routeBudget_ -= 1;
            if (planRoute(me, u.lastSteer, u.object, u.detour)) u.detourGoal = u.lastSteer, u.detourIdx = 0;
            if (tacDbg) SDL_Log("ai %d route to %.0f,%.0f: %zu waypoints", u.object, u.lastSteer.x, u.lastSteer.z, u.detour.size());
        }
        if (u.braked) in.throttle = 0, in.strafe = 0, in.run = false;
        if (!u.mobile) {  // turrets only rotate
            float y = u.body.yaw() + in.turn * 1.2f * dt;
            if (u.arc) y = u.arcHome + std::clamp(wrap(y - u.arcHome), u.arcMin, u.arcMax);
            u.body.setYaw(y);
            if (u.target < 0) u.gunPitch *= std::max(0.f, 1.f - dt);  // the gun settles back level
            scene_->setObjectPitch(u.object, u.gunPitch);               // pitch joint frame (frm-xa)
            if (getenv("HG2_TURRETDBG") && int(u.clock) != int(u.clock - dt))
                SDL_Log("turret %d target %d yaw %.2f home %.2f arc %d [%.2f %.2f] pitch %.2f [%.2f %.2f]", u.object, u.target, u.body.yaw(), u.arcHome, u.arc, u.arcMin, u.arcMax, u.gunPitch, u.pitchMin, u.pitchMax);
            in = PlayerInput{};
        }
        if (getenv("HG2_UNITDBG") && u.object == atoi(getenv("HG2_UNITDBG")) && int(u.clock * 2) != int((u.clock - dt) * 2))
            SDL_Log("unit %d thr %.2f run %d turn %.2f braked %d bodySpeed %.2f walk %d mode %d ord %zu drive %d",
                    u.object, in.throttle, in.run, in.turn, u.braked, u.body.speed(), u.body.canWalk(), u.mode, u.orders.size(), u.drive);
        u.body.update(dt, in);
        combat_->setPosture(u.object, u.body.postureCode());
    }
}


// ---- route planning: the original's MOVETO pathfinder (FUN_100bf020 → FUN_100c3190 search, FUN_100c26a0 expand,
// FUN_100c2520 walkability, FUN_100c3530 straight-line test), as decoded:
//  * a 4 m grid (DAT_102689d0) with 8 neighbours; a step costs its squared length (16, diagonals 32) plus the squared
//    height change, and under water (depth + 1) × the step when the request asks for the water penalty;
//  * f = g + the squared straight-line distance to the goal; nodes are expanded best-f first, at most 2048 of them
//    (128 a frame in the original); a node within the goal radius (3 m) ends the search, otherwise the path runs
//    to the best node found (a partial path, result 3);
//  * a node is blocked by a solid object (the occupancy grid's flags), by water deeper than the unit's bounding
//    radius (object +0x98) unless the request lifts the depth limit, and by ground whose triangle normal has
//    |n·up| < the unit's slope cosine (the follower's +0xa4, FUN_100bff80);
//  * the path is then shortened by straight-line tests in 2 m steps.
// Per unit class (MOVETO of each class, see docs/FORMATS.md): the slope limit is 30° (cos 0.866, pushed as a constant)
// for gears, infantry, mounts, chicken walkers, frames and hovers; Jeeps (0x4e) use their +0x27c, cos 20°. Flags:
// gears 0 (8 = no depth limit when Amphibious), Jeeps and mounts 0, hovers 0x38 (no depth limit, no water penalty,
// nodes on the water surface).
// The port's occupancy test is Scene::collide.
namespace {
constexpr float kCell = 4.f;
}
AiSystem::MoveRules AiSystem::moveRules(int self) const {
    MoveRules r;
    int type = self >= 0 && size_t(self) < scene_->mission().objects.size() ? scene_->mission().objects[size_t(self)].type : 0x48;
    r.slopeCos = type == 0x4e ? 0.9397f : 0.866f;     // FUN_1006c700 → FUN_10049db0(20); 0x3f5db22d
    bool hover = type == 0x4f;
    bool amphibious = (type == 0x48 || type == 0x50 || type == 0x52 || type == 0x53) && combat_ && combat_->amphibious(self);
    r.depthLimit = hover || amphibious ? 1e9f : std::max(1.f, scene_->objectRadius(self));
    r.waterCost = !hover;
    r.onSurface = hover;
    return r;
}
bool AiSystem::walkable(float x, float z, const MoveRules& r) const {
    if (scene_->mission().hasWater) {
        float depth = scene_->waterLevel() - scene_->groundHeight(x, z);
        if (depth > 1e-5f && depth > r.depthLimit) return false;
    }
    return std::fabs(scene_->groundNormal(x, z).y) >= r.slopeCos;
}
bool AiSystem::lineBlocked(const Vec3& from, const Vec3& to, int self) const {
    Vec3 d = flat(to) - flat(from);
    float len = length(d);
    if (len < 1.f) return false;
    MoveRules rules = moveRules(self);
    int n = int(len / 2.f) + 1;   // FUN_100c3530: 2 m steps
    for (int i = 1; i <= n; ++i) {
        Vec3 p = from + d * (float(i) / float(n));
        if (!walkable(p.x, p.z, rules)) return true;
        Vec3 q(p.x, scene_->groundHeight(p.x, p.z), p.z);
        if (scene_->collide(q, 2.5f, 8.f, self)) return true;
    }
    return false;
}
bool AiSystem::planRoute(const Vec3& from, const Vec3& to, int self, std::vector<Vec3>& out) const {
    out.clear();
    float x0 = std::min(from.x, to.x) - 96.f, z0 = std::min(from.z, to.z) - 96.f;
    int W = std::min(256, int((std::max(from.x, to.x) + 96.f - x0) / kCell) + 1);
    int H = std::min(256, int((std::max(from.z, to.z) + 96.f - z0) / kCell) + 1);
    auto cellOf = [&](const Vec3& p, int& cx, int& cz) {
        cx = std::clamp(int((p.x - x0) / kCell), 0, W - 1), cz = std::clamp(int((p.z - z0) / kCell), 0, H - 1);
    };
    int sx, sz, gx, gz;
    cellOf(from, sx, sz), cellOf(to, gx, gz);
    std::vector<signed char> kind(size_t(W) * H, -1);  // -1 unknown, 0 free, 1 blocked
    std::vector<float> hgt(size_t(W) * H, 0.f);
    float water = scene_->mission().hasWater ? scene_->waterLevel() : -1e9f;
    MoveRules rules = moveRules(self);
    std::vector<float> depth(size_t(W) * H, 0.f);
    auto centre = [&](int cx, int cz) { return Vec3(x0 + (cx + 0.5f) * kCell, 0, z0 + (cz + 0.5f) * kCell); };
    auto classify = [&](int cx, int cz) {
        size_t i = size_t(cz) * W + cx;
        if (kind[i] < 0) {
            Vec3 c = centre(cx, cz);
            float h = scene_->groundHeight(c.x, c.z);
            hgt[i] = rules.onSurface ? std::max(h, water) : h;  // request flag 0x20 (+0x3b): nodes on the water surface
            depth[i] = std::max(0.f, water - h);
            Vec3 q(c.x, h, c.z);
            kind[i] = !walkable(c.x, c.z, rules) || scene_->collide(q, 2.5f, 8.f, self) ? 1 : 0;  // no surface test
        }
        return kind[i];
    };
    Vec3 goalP(to.x, scene_->groundHeight(to.x, to.z), to.z);
    auto hcost = [&](int cx, int cz, float h) {  // squared distance to the goal
        Vec3 c = centre(cx, cz);
        float dx = c.x - goalP.x, dy = h - goalP.y, dz = c.z - goalP.z;
        return dx * dx + dy * dy + dz * dz;
    };
    std::vector<float> g(size_t(W) * H, 1e30f);
    std::vector<int> parent(size_t(W) * H, -1);
    std::vector<char> closed(size_t(W) * H, 0);
    using QE = std::pair<float, int>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> open;
    int start = sz * W + sx;
    classify(sx, sz);
    g[start] = 0;
    open.push({hcost(sx, sz, hgt[start]), start});
    int expanded = 0, best = start;
    float bestF = 1e30f;
    bool reached = false;
    while (!open.empty() && expanded < 2048) {
        auto [f, cur] = open.top();
        open.pop();
        if (closed[cur]) continue;
        closed[cur] = 1;
        int cx = cur % W, cz = cur / W;
        float hc = hcost(cx, cz, hgt[cur]);
        if (hc <= 9.f || (cx == gx && cz == gz)) { best = cur, reached = true; break; }  // within 3 m
        if (f < bestF) bestF = f, best = cur;
        ++expanded;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dz) continue;
                int nx = cx + dx, nz = cz + dz;
                if (nx < 0 || nz < 0 || nx >= W || nz >= H) continue;
                int ni = nz * W + nx;
                if (closed[ni]) continue;
                int k = classify(nx, nz);
                if (k == 1) continue;
                float step = (dx && dz) ? 2.f * kCell * kCell : kCell * kCell;
                float dh = hgt[ni] - hgt[cur];
                float c = g[cur] + step + dh * dh;
                if (rules.waterCost && depth[ni] > 0) c += (depth[ni] + 1.f) * step;
                if (c < g[ni]) g[ni] = c, parent[ni] = cur, open.push({c + hcost(nx, nz, hgt[ni]), ni});
            }
    }
    if (best == start) return false;
    std::vector<int> cells;
    for (int c = best; c >= 0; c = parent[c]) cells.push_back(c);
    std::reverse(cells.begin(), cells.end());
    Vec3 last = from;
    for (size_t i = 1; i < cells.size(); ++i) {
        Vec3 c = centre(cells[i] % W, cells[i] / W);
        bool end = i + 1 == cells.size();
        Vec3 nxt = end ? c : centre(cells[i + 1] % W, cells[i + 1] / W);
        if (end || lineBlocked(last, nxt, self)) out.push_back(c), last = c;
    }
    if (reached) out.back() = Vec3(to.x, 0, to.z);
    return !out.empty();
}
}  // namespace hg2
