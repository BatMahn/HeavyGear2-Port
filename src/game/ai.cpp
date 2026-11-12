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
// How far a unit notices enemies at each team alert level (defcon 0..5) relative to its sensor range.
constexpr float kAlert[6] = {0.25f, 0.45f, 0.65f, 0.85f, 1.0f, 1.25f};
}  // namespace

void AiSystem::init(const FileSystem& fs, Scene& scene, Combat& combat, const Player& player) {
    scene_ = &scene;
    combat_ = &combat;
    player_ = &player;
    // teams start relaxed (defcon 0, FUN_100b89e0(0)) and grow alert from what their units see and suffer
    for (int t = 0; t < 16; t++) defcon_[t] = 0, minDefcon_[t] = 0, maxDefcon_[t] = 5, alert_[t] = 0;
    fs_ = &fs;
    for (size_t i = 0; i < scene.mission().objects.size(); i++)
        if (int(i) != player.object()) addUnit(int(i));
    SDL_Log("AI: %zu units, %zu paths", units_.size(), scene.mission().paths.size());
}

bool AiSystem::addUnit(int index) {
    const MissionObject& o = scene_->mission().objects[size_t(index)];
    if (o.team < 100) return false;
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
    u.detect = 220.f + 15.f * float(u.body.def().sensors);
    u.home = u.body.position();
    combat_->addUnit(index, u.body.def());
    u.think = float(index % 10) * 0.05f;
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
    Unit* u = find(o);
    if (!u) return;
    float maxKph = std::max(1.f, u->body.def().maxRunSpeed * 3.6f);
    u->speedFrac = std::max(0.05f, std::min(1.f, kph / maxKph));
}
void AiSystem::attack(int o, int t) {
    if (Unit* u = find(o)) u->forced = t, u->target = t, u->asleep = false, u->awake = true;
}
void AiSystem::notifyRange(int o, float r) {
    if (Unit* u = find(o)) u->enemyRange = std::max(0.f, r);
}

void AiSystem::command(int o, const std::string& c, float a, float b, float cc, const std::string& s) {
    (void)b, (void)cc;
    Unit* u = find(o);
    if (!u) return;
    if (c == "visibility") u->detect = std::max(0.f, a);
    else if (c == "friendRange") u->friendRange = std::max(0.f, a);
    else if (c == "canTarget") {
        u->canTarget = a != 0;
        if (!u->canTarget) u->target = -1, u->forced = -1;
    } else if (c == "movementMode") u->body.setSkates(int(a) == 3), u->scriptStance = true;
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
    else if (c == "autopilot") u->asleep = false, u->forced = -1, u->drive = 0, u->braked = false;  // back to the brain's own behaviour
}

void AiSystem::hitBy(int o, int shooter) {
    Unit* u = find(o);
    if (!u || shooter < 0 || shooter == o) return;
    u->attacker = shooter;
    if (u->canTarget && teamOf(shooter) != u->team) u->awake = true, u->braked = false;
    if (teamOf(shooter) != u->team) raiseAlert(u->team - 99, 3.f);
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
    if (Unit* u = find(object)) u->leader = leader, u->slot = slot, u->awake = true, u->autoSleep = false;
}

void AiSystem::squadOrder(int leader, int order, int object, const Vec3& pos) {
    for (auto& u : units_) {
        if (u.leader != leader || !combat_->alive(u.object)) continue;
        u.order = order, u.orderObj = object, u.orderPos = pos;
        u.braked = false;
        if (order == OrderAttack) u.forced = object;
        else u.forced = -1;
        if (order == OrderHold) u.orderPos = u.body.position();
        if (order == OrderRegroup) u.disengage = 6.f, u.target = -1, u.order = OrderFollow;
        if (order == OrderGoto) {  // spread round the nav point
            int k = int(&u - units_.data());
            u.orderPos = pos + Vec3(std::sin(float(k) * 2.1f), 0, std::cos(float(k) * 2.1f)) * 15.f;
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
    if (ref == 1) return u->enemyNear >= 0 ? u->enemyNear : u->target;
    if (ref == 2) return u->friendNear;
    if (ref == 5) return u->attacker >= 0 ? u->attacker : u->target;
    return -1;
}

void AiSystem::raiseAlert(int team, float points) {
    if (team < 0 || team >= 16) return;
    alert_[team] += points;
    // escalate a level each time the points pass the next threshold (FUN_100b8xxx), clamped to the script limits
    while (defcon_[team] < maxDefcon_[team] && alert_[team] > float(defcon_[team] + 1) * 6.f) defcon_[team]++;
}

void AiSystem::setDefcon(int team, int level, int mode) {
    if (team < 0 || team >= 16) return;
    level = std::max(0, std::min(5, level));
    if (mode == 1) {
        minDefcon_[team] = level;
        defcon_[team] = std::max(defcon_[team], level);
    } else if (mode == 2) {
        maxDefcon_[team] = level;
        defcon_[team] = std::min(defcon_[team], level);
    } else {
        defcon_[team] = std::max(minDefcon_[team], std::min(maxDefcon_[team], level));
    }
    alert_[team] = std::max(alert_[team], float(defcon_[team]) * 6.f + 30.f);  // a scripted alert lasts a while
}
int AiSystem::defcon(int team) const { return team >= 0 && team < 16 ? defcon_[team] : 6; }

void AiSystem::dump() const {
    for (auto& u : units_) {
        Vec3 p = u.body.position();
        SDL_Log("  unit %d %s team %d at %.0f,%.0f %s%s path %zu/%zu tgt %d hp %.2f", u.object,
                scene_->mission().objects[size_t(u.object)].name.c_str(), u.team, p.x, p.z, u.asleep ? "asleep " : "",
                u.inCombat ? "combat " : "", u.pathIdx, u.path.size(), u.target, combat_->health(u.object));
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
    routeBudget_ = getenv("HG2_NOROUTE") ? 0.f : 2.f;  // route plans per frame
    for (int t = 0; t < 16; t++) {  // alerts fade: a team calms down a level at a time
        alert_[t] = std::max(0.f, alert_[t] - dt * 0.25f);
        if (defcon_[t] > minDefcon_[t] && alert_[t] < float(defcon_[t]) * 3.f) defcon_[t]--, alert_[t] = float(defcon_[t] + 1) * 4.5f;
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
                const Surface* sf = scene_->surfaceAt(me.x, me.z);
                // never doze on lava, nor while hunting the user or walking a patrol
                const std::string& nc = u.nonCombatTactic;
                bool roaming = nc.find("hunt") != std::string::npos || nc.find("patrol") != std::string::npos ||
                               nc.find("freerange") != std::string::npos || nc.find("pathfollow") != std::string::npos ||
                               (u.assigned && u.route.size() > 1);
                u.dozing = dist > 300.f && !inView && u.attacker < 0 && !(sf && sf->damage > 0) && !roaming;
            }
        } else {
            u.dozing = false;
        }
        if (u.dozing) continue;
        u.clock += dt;
        int sideIdx = u.team - 99, alert = defcon(sideIdx);
        u.body.speedScale = combat_->mobility(u.object), u.body.turnScale = combat_->manoeuvre(u.object);
        float sensorMul = combat_->sensorsOk(u.object) ? 1.f : 0.5f;
        u.think -= dt;
        if (u.think <= 0) {  // perception 4 times a second
            u.think = 0.25f;
            // nearest enemy / friend for the notify-range events (FUN_100b9fe0)
            float bestE = 1e9f, bestF = 1e9f;
            int nearE = -1, nearF = -1;
            auto look = [&](int obj, int team, const Vec3& p) {
                if (obj == u.object || !combat_->alive(obj)) return;
                float d = length(p - me);
                if (team == u.team) {
                    if (d < bestF) bestF = d, nearF = obj;
                } else if (team >= 100 && d < bestE) {
                    bestE = d, nearE = obj;
                }
            };
            if (playerAlive) look(player_->object(), playerTeam, player_->position());
            for (auto& o : units_) look(o.object, o.team, o.body.position());
            if (u.enemyRange > 0) {
                bool in = nearE >= 0 && bestE <= u.enemyRange;
                if (in != u.enemyIn) event(u.object, in ? EV_ENEMY_IN : EV_ENEMY_OUT);
                u.enemyIn = in;
                u.enemyNear = in ? nearE : -1;
            }
            if (u.friendRange > 0) {
                bool in = nearF >= 0 && bestF <= u.friendRange;
                if (in != u.friendIn) event(u.object, in ? EV_FRIEND_IN : EV_FRIEND_OUT);
                u.friendIn = in;
                u.friendNear = in ? nearF : -1;
            }
            // targets: Attack() order, else the closest visible enemy within the alert-scaled sensor range
            u.target = -1;
            if (u.disengage > 0) u.disengage -= 0.25f;
            if (u.order == OrderAttack && (u.orderObj < 0 || !combat_->alive(u.orderObj))) u.order = OrderFollow, u.forced = -1;
            if (u.order == OrderDefend && (u.orderObj < 0 || !combat_->alive(u.orderObj))) u.order = OrderFollow;
            if (!u.asleep && u.canTarget && u.disengage <= 0) {
                if (u.forced >= 0 && combat_->alive(u.forced) && !dozing(u.forced)) u.target = u.forced;
                float best = u.detect * sensorMul * kAlert[std::max(0, std::min(5, alert))] * (u.awake ? 1.5f : 1.f);
                if (u.team != playerTeam) best *= 0.85f + 0.3f * difficulty_;  // harder pilots spot you sooner
                auto consider = [&](int obj, int team, const Vec3& p) {
                    if (team == u.team || team < 100 || !combat_->alive(obj) || dozing(obj)) return;
                    float d = length(p - me);
                    // sensors against stealth: compare against the range this target can be seen at
                    float k = combat_->detectFactor(u.object, obj);
                    if (u.order == OrderDefend) {  // only what threatens the defended unit
                        Vec3 dp;
                        if (positionOf(u.orderObj, dp) && length(p - dp) > 180.f) return;
                    }
                    if (d < best * k && scene_->terrainRay(me + Vec3(0, 5, 0), p + Vec3(0, 4, 0)) > 1.f) best = d / k, u.target = obj;
                };
                if (u.target < 0) {
                    if (playerAlive) consider(player_->object(), playerTeam, player_->position());
                    // HuntUser units go for the user and whoever shoots at them, nobody else
                    bool hunter = u.nonCombatTactic.find("hunt") != std::string::npos;
                    for (auto& o : units_)
                        if (&o != &u && !hunter) consider(o.object, o.team, o.body.position());
                }
                // an attacker is engaged even beyond the sensor range
                if (u.target < 0 && u.attacker >= 0 && combat_->alive(u.attacker) && teamOf(u.attacker) != u.team) {
                    Vec3 ap;
                    if (positionOf(u.attacker, ap) && length(ap - me) < u.detect * 2.f) u.target = u.attacker;
                }
                if (u.target >= 0) u.awake = true;
            }
            // combat starts with a target and ends after 3 s without one
            // a team grows alert when its units see the user or come under fire
            if (u.target >= 0) u.calm = 0;
            if (u.target >= 0 && (u.target == player_->object() || u.attacker >= 0)) raiseAlert(sideIdx, 0.4f);
            else u.calm += 0.25f;
            bool combat = u.target >= 0 || (u.inCombat && u.calm < 3.f);
            if (combat != u.inCombat) event(u.object, combat ? EV_COMBAT_START : EV_COMBAT_END);
            u.inCombat = combat;
        }
        if (u.asleep) {
            u.body.update(dt, PlayerInput{});
            continue;
        }
        PlayerInput in;
        Vec3 tp;
        auto steerTo = [&](const Vec3& p, float& err) {
            u.lastSteer = p;
            Vec3 aim = p;
            if (!u.detour.empty()) {
                if (length(flat(u.detourGoal) - flat(p)) > 12.f || u.ord.kind == 10) u.detour.clear();
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
        // escorts' vehicles and unarmed units keep to their path and only shoot on the move
        // weapons hold at the lowest alert level: only the user, an attacker, or a squad mate's fight opens fire
        bool weaponsFree = alert >= 1 || u.attacker >= 0 || u.leader >= 0 || u.target == player_->object();
        Vec3 leaderPos;
        bool leash = false;  // squad mates break off a fight that drags them far from the leader
        float leashR = u.order == OrderFireAtWill ? 600.f : u.order == OrderHold || u.order == OrderGoto ? 1e9f : 220.f;
        if (u.leader >= 0 && positionOf(u.leader, leaderPos) && length(leaderPos - me) > leashR && u.forced < 0) leash = true;
        if (u.order == OrderFireAtWill) weaponsFree = true;
        if (leash) u.target = -1;
        bool pathFirst = !u.path.empty() && u.pathIdx < u.path.size() && u.forced < 0 &&
                         (combat_->range(u.object) <= 0 || (u.team == playerTeam && u.type != 0x48));
        if (pathFirst && u.target >= 0 && positionOf(u.target, tp)) {
            Vec3 d = tp - me, fwd(-std::sin(u.body.yaw()), 0, -std::cos(u.body.yaw()));
            float dl = length(d);
            if (weaponsFree && dl > 1.f && dl < combat_->range(u.object) && dot(d * (1.f / dl), fwd) > 0.97f)
                combat_->fire(u.object, me + Vec3(0, 3.f, 0) + fwd * 2.f, tp + Vec3(0, 3.5f, 0));
        }
        // PathFollow1 (FUN_100ae440): from the nearest node of the unit's path, MOVE_DIRECT to a neighbour chosen at
        // random (50/50 back or on), clamped at the ends, so the unit wanders up and down its path for good.
        // PathFollow2 (FUN_100ae910, mode 0): walk the nodes in turn, looping, each with a random offset.
        // With no non-combat tactic of its own, a unit that has an assigned path gets PathFollow1 from the tactic table
        // (FUN_100b9510 → FUN_100dd910 picks the first registered tactic that applies).
        bool ncDefault = u.nonCombatTactic.empty() || u.nonCombatTactic == "default" || u.nonCombatTactic == "none";
        if (u.mobile && u.target < 0 && u.path.empty() && u.route.size() > 1 && u.leader < 0 && !u.braked &&
            (u.nonCombatTactic.find("pathfollow") != std::string::npos || (ncDefault && u.assigned))) {
            int n = int(u.route.size());
            if (u.routeIdx < 0) {  // start at the nearest node
                float best = 1e30f;
                for (int i = 0; i < n; ++i)
                    if (float d = length(flat(u.route[i]) - flat(me)); d < best) best = d, u.routeIdx = i;
            } else if (u.nonCombatTactic.find("pathfollow2") != std::string::npos) {
                u.routeIdx = (u.routeIdx + 1) % n;
            } else {
                std::uniform_real_distribution<float> R(0.f, 1.f);
                u.routeIdx = std::clamp(u.routeIdx + (R(rng_) <= 0.5f ? -1 : 1), 0, n - 1);
            }
            Vec3 p = u.route[u.routeIdx];
            if (u.nonCombatTactic.find("pathfollow2") != std::string::npos) {
                std::uniform_real_distribution<float> R(-1.f, 1.f);
                p = p + Vec3(R(rng_), 0, R(rng_)) * 8.f;
            }
            u.path = {p}, u.pathIdx = 0, u.hop = true;
            if (getenv("HG2_TACDBG")) SDL_Log("ai %d pathfollow node %d/%d at %.0f,%.0f", u.object, u.routeIdx, n, p.x, p.z);
        }
        {   // tactic switch (combat <-> non-combat) starts the new tactic from its first step
            bool combatNow = !pathFirst && u.target >= 0;
            if (combatNow != u.engaged) u.engaged = combatNow, u.ord = Unit::Order{}, u.step = 0;
        }
        if (!pathFirst && u.target >= 0 && positionOf(u.target, tp)) {
            float err, dist = steerTo(tp, err);
            in.turn = 0;
            float range = std::max(40.f, combat_->chooseWeapon(u.object, dist));
            u.visible = scene_->terrainRay(me + Vec3(0, 5, 0), tp + Vec3(0, 4, 0)) > 0.98f;
            bool canFire = u.visible && dist < range;
            std::uniform_real_distribution<float> R(0.f, 1.f);
            // ---- choose a combat tactic: the script's, else one of the unit class's from the tactic table
            std::string tac = u.combatTactic;
            if (tac.empty() || tac == "default" || tac == "none") {
                if (u.tactic.empty()) {
                    float r = R(rng_);
                    bool melee = combat_->hasMelee(u.object);
                    if (!u.mobile) u.tactic = "turretcombat1";
                    else if (u.type == 0x4e || u.type == 0x4f) u.tactic = r < 0.5f ? "tankattack" : "jeeppursue";
                    else if (u.type == 0x50) u.tactic = r < 0.6f ? "infantrycombat1" : "infantrycover";
                    else if (melee && r < 0.15f) u.tactic = "gearcombathand2hand";
                    else if (r < 0.55f) u.tactic = "gearcombat1";
                    else if (r < 0.7f) u.tactic = "gearcombat3";
                    else if (r < 0.85f) u.tactic = "gearflankleft";
                    else u.tactic = "gearflankright";
                    u.step = 0, u.count = 3, u.ord = Unit::Order{};
                }
                tac = u.tactic;
            } else if (tac != u.tactic) {
                u.tactic = tac, u.step = 0, u.count = 3, u.ord = Unit::Order{};
            }
            if (u.order == OrderHold || u.order == OrderGoto) tac = "gearcombat2", u.home = u.orderPos;  // hold the ground
            auto post = [&](int kind, const Vec3& p, float speed, float timer = 0) {
                if (u.ord.kind != kind || length(flat(u.ord.pos) - flat(p)) > 4.f) u.ord = Unit::Order{};  // a new order
                u.ord.kind = kind, u.ord.pos = p, u.ord.speed = speed, u.ord.timer = timer;
            };
            bool busy = u.ord.kind != 0;
            auto cancel = [&]() { u.ord = Unit::Order{}; busy = false; };
            auto around = [&](const Vec3& c, float r, float ang) { return c + Vec3(std::sin(ang), 0, std::cos(ang)) * r; };
            float bearingFromTarget = std::atan2((me - tp).x, (me - tp).z);
            // ---- tactic think (the original's per-tactic Think state machines, paraphrased)
            if (u.mobile && u.route.size() > 1 && tac.find("pathfollow2") != std::string::npos) {
                // PathFollow2 in combat (FUN_100aed10): keep to the path, MOVE_DIRECT (shooting on the move) to the
                // node nearest the target that is still at least the free-range radius from it, preferring the
                // neighbouring nodes (×0.8, FUN_100ae820); once there, drift round that node within the radius
                if (!busy) {
                    int n = int(u.route.size()), cur = std::max(0, u.routeIdx), pick = cur;
                    float bestD = 1e30f;
                    for (int i = 0; i < n; ++i) {
                        float d = length(flat(u.route[i]) - flat(tp));
                        if (d < u.freeRadius) continue;
                        if (std::abs(i - cur) < 2) d *= 0.8f;
                        if (d < bestD) bestD = d, pick = i;
                    }
                    if (pick != cur) pick = cur + (pick > cur ? 1 : -1);  // one node at a time along the path
                    u.routeIdx = pick;
                    Vec3 p = u.route[pick];
                    if (pick == cur && u.freeRadius > 0) p = around(p, u.freeRadius * R(rng_), R(rng_) * 6.2832f);
                    if (length(flat(p) - flat(me)) > 6.f) post(2, p, 3.f);
                    else if (canFire) post(10, tp, 0, 2.f);
                }
            } else if (!u.mobile || tac.find("turret") != std::string::npos) {
                if (!busy && canFire) post(10, tp, 0, 5.f);  // TurretCombat1: 5 s volleys
            } else if (tac == "standandshoot") {               // StandAndShoot (FUN_100a8ba0)
                if (!busy) post(10, tp, 0, 4.f);
            } else if (tac.find("gearcombat2") != std::string::npos || tac.find("chickenwalkercombat2") != std::string::npos ||
                       tac.find("mountcombat1") != std::string::npos) {
                // GearCombat2 (FUN_100a8dd0): from the home spot, turn and shoot what's in sight, else go back home
                if (!busy) {
                    if (canFire) post(10, tp, 0, 3.f);
                    else if (length(u.home - me) > 8.f) post(2, u.home, 3.f), u.step = 2;
                } else if (u.step == 2 && canFire) cancel(), u.step = 0;
            } else if (tac.find("combat3") != std::string::npos || tac.find("circle") != std::string::npos) {
                // GearCombat3 / HoverCircle (FUN_100a90e0): every 2 s a new spot - home without a shot, a random
                // point within 32 m when 32 m or more out, else away from the target
                if ((u.replan -= dt) <= 0 || !busy) {
                    u.replan = 2.f;
                    if (!u.visible) post(2, u.home, 3.f);
                    else if (dist >= 32.f) post(2, around(me, 32.f, R(rng_) * 6.2832f), 3.f);
                    else post(2, around(me, 32.f, bearingFromTarget + (R(rng_) - 0.5f) * 1.6f), 3.f);
                }
            } else if (tac.find("flank") != std::string::npos || tac == "frontal") {
                // GearFlankLeft / Right (FUN_100a9770 / FUN_100a9ee0): get round to +-75 degrees off the target's
                // bearing, then turn in and fire a 5 s volley
                float ang = tac.find("left") != std::string::npos ? 75.f : tac.find("right") != std::string::npos ? -75.f : 0.f;
                if (tac == "flankleft1") ang = 45.f;
                if (tac == "flankright1") ang = -45.f;
                if (tac == "flankleft2") ang = 90.f;
                if (tac == "flankright2") ang = -90.f;
                if (!busy) {
                    if (u.step == 0) post(2, around(tp, std::min(range * 0.7f, 120.f), bearingFromTarget + ang * 0.017453f), 3.f), u.step = 1;
                    else if (canFire) post(10, tp, 0, 5.f), u.step = 0;
                    else u.step = 0;
                }
            } else if (tac.find("hand2hand") != std::string::npos) {
                // GearCombatHand2Hand (FUN_100aacc0): close to 8 m and fight
                if (!busy || u.ord.kind == 2) {
                    if (dist > 8.f) post(2, tp, 3.f);
                    else post(10, tp, 0, 2.f);
                }
            } else if (tac.find("tankattack") != std::string::npos) {
                // TankAttack (FUN_100ab040): in sight 80 % stand and fire 5 s, 20 % reposition; else drive at it
                if (!busy) {
                    if (canFire) {
                        if (R(rng_) > 0.8f) post(2, around(me, 40.f, R(rng_) * 6.2832f), 3.f);
                        else post(10, tp, 0, 5.f);
                    } else post(2, tp, 3.f), u.step = 2;
                } else if (u.step == 2 && canFire) cancel(), u.step = 0;
            } else if (tac.find("pursue") != std::string::npos || tac == "chaseenemy") {
                if (!busy) post(2, tp, 3.f);  // GearPursue1 / JeepPursue (FUN_100af9b0): drive at the target
            } else if (tac.find("droneattack") != std::string::npos) {
                // DroneAttack1 (FUN_100b06e0): runs in, breaks off after a random few seconds or when close, repeats
                if ((u.tacTimer -= dt) <= 0) u.step = u.step == 1 ? 2 : 1, u.tacTimer = 3.f + R(rng_) * 4.f;
                if (u.step == 1) post(2, tp, 4.f);
                else post(2, around(tp, 120.f, bearingFromTarget), 4.f);
                if (dist < 25.f) u.step = 2;
            } else if (tac.find("cover") != std::string::npos) {
                // InfantryCover (FUN_100a9610): get down where it is and shoot
                u.body.setPosture(0);
                if (!busy && canFire) post(10, tp, 0, 4.f);
            } else if (tac.find("runaway") != std::string::npos) {
                if (!busy) post(2, around(me, 30.f, bearingFromTarget), 4.f);  // DefaultRunAway (FUN_100ab550)
            } else {
                // GearCombat1 / InfantryCombat1 / FrameCombat1 / HoverCombat1 (FUN_100a84d0): turn to the target;
                // no shot: close in; in reach: under 32 m 10 % a volley, else a 64 m sidestep round it at 90
                // degrees; from 32 m on 20 % a volley, else a run across it
                if (!busy) {
                    if (std::fabs(err) > 0.6f) post(6, tp, 3.f);
                    else if (!canFire) post(2, tp, 3.f), u.step = 2;
                    else {
                        float r = R(rng_);
                        if (dist >= 32.f && r < 0.2f) post(10, tp, 0, 2.f);
                        else if (dist < 32.f && r < 0.1f) post(10, tp, 0, 2.f);
                        else if (dist < 32.f) post(2, around(tp, 64.f, bearingFromTarget + (R(rng_) < 0.5f ? 1.5708f : -1.5708f)), 4.f);
                        else {
                            Vec3 side(std::cos(bearingFromTarget), 0, -std::sin(bearingFromTarget));
                            post(2, tp + (me - tp) * 0.5f + side * (R(rng_) < 0.5f ? 64.f : -64.f), 4.f);
                        }
                        u.step = 0;
                    }
                } else if (u.step == 2 && canFire) cancel(), u.step = 0;
            }
            // retreat when badly hurt (gears)
            if (combat_->health(u.object) < 0.25f && u.type == 0x48 && tac != "standandshoot" && u.mobile && u.order != OrderHold &&
                (u.ord.kind != 2 || length(u.ord.pos - tp) < length(me - tp)))
                post(2, around(me, 60.f, bearingFromTarget), 4.f);
            // no line of fire for a while: move to find one
            if (u.blocked > 1.5f && u.mobile && u.order != OrderHold && u.ord.kind == 10)
                post(2, around(me, 25.f, bearingFromTarget + (R(rng_) < 0.5f ? 1.2f : -1.2f) + 3.1416f), 3.f), u.blocked = 0;
            // ---- execute the order
            float aimErr = err;
            switch (u.ord.kind) {
                case 2: {  // MOVETO: speed 3 walking pace, 4 and up running
                    float e2, d2 = steerTo(u.ord.pos, e2);
                    if (d2 < 6.f) { u.ord = Unit::Order{}; break; }
                    if (std::fabs(e2) < 1.2f) in.throttle = 1, in.run = u.ord.speed >= 4.f || d2 > 60.f;
                    break;
                }
                case 6: {  // TURNTO
                    float e2;
                    steerTo(u.ord.pos, e2);
                    if (std::fabs(e2) < 0.1f) u.ord = Unit::Order{};
                    break;
                }
                case 10:   // FIREWPN volley: stand, face it, shoot for the order's time
                    steerTo(tp, aimErr);
                    if ((u.ord.timer -= dt) <= 0) u.ord = Unit::Order{};
                    break;
                default: steerTo(tp, aimErr); break;
            }
            // the gunner: the torso twists toward the target so moving gears shoot too
            u.body.twist = std::clamp(err, -1.2f, 1.2f);
            float gunErr = err - u.body.twist;
            if (weaponsFree && std::fabs(gunErr) < 0.2f && canFire) {
                Vec3 fwd(-std::sin(u.body.yaw() + u.body.twist), 0, -std::cos(u.body.yaw() + u.body.twist)), mz;
                if (!combat_->muzzle(u.object, mz)) mz = me + Vec3(0, 5.f, 0) + fwd * 2.f;
                // lead a moving target, scattered by the gunner's fire control
                Vec3 vel = (tp - u.lastTargetPos) * (1.f / std::max(0.016f, dt));
                if (u.lastTarget != u.target || length(vel) > 60.f) vel = Vec3();
                float flight = dist / 200.f;
                std::uniform_real_distribution<float> U(-1.f, 1.f);
                float spread = std::max(0.3f, 2.5f - 0.5f * float(u.body.def().fireControl)) * dist / 150.f *
                               combat_->aimSpread(u.object, u.target) * (u.team != playerTeam ? 1.4f - 0.8f * difficulty_ : 1.f);
                if (u.ord.kind == 2 && in.throttle > 0) spread *= 1.5f;  // firing on the move
                Vec3 aim = tp + Vec3(0, 3.5f, 0) + vel * flight + Vec3(U(rng_), U(rng_) * 0.5f, U(rng_)) * spread;
                // hold fire into a hill or a friend in the way
                float tt = 2.f;
                Vec3 clear = tp + Vec3(0, 3.5f, 0);
                int hit = scene_->raycast(mz, clear, u.object, tt);
                bool terrain = scene_->terrainRay(mz, clear) < 0.97f;
                bool friendInWay = hit >= 0 && hit != u.target && tt < 0.97f && teamOf(hit) == u.team;
                if (terrain || friendInWay) u.blocked += dt;
                else {
                    u.blocked = 0, combat_->setAimTarget(u.object, u.target);
                    bool f = combat_->fire(u.object, mz, aim);
                    if (f && getenv("HG2_TACDBG")) SDL_Log("tac %d fires at %d (%.0f m)", u.object, u.target, dist);
                }
            } else {
                u.blocked = std::max(0.f, u.blocked - dt * 0.5f);
            }
            (void)aimErr;
            if (getenv("HG2_TACDBG") && int(u.clock * 2) != int((u.clock - dt) * 2))
                SDL_Log("tac %d %s step %d ord %d dist %.0f range %.0f vis %d err %.2f free %d", u.object, tac.c_str(), u.step, u.ord.kind, dist,
                        range, int(u.visible), err, int(weaponsFree));
            u.lastTargetPos = tp, u.lastTarget = u.target;
        } else if (u.drive != 0) {  // Forward / Backward
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
                    in.throttle = 1, in.run = u.speedFrac > 0.5f && dist > 40.f;
                }
            }
        } else if (u.mobile && u.leader >= 0 && (u.order == OrderHold || u.order == OrderGoto)) {
            // SquadMateGuard (FUN_100ae0e0): MOVETO the guard point while that brings it nearer; STAND once it
            // stops gaining (blocked, or there). A new order resets the best distance.
            float err, dist = steerTo(u.orderPos, err);
            if (length(flat(u.orderPos) - flat(u.goal)) > 1.f) u.goal = u.orderPos, u.guardBest = 1e30f, u.step = 0;
            if ((u.tacTimer -= dt) <= 0) {  // judged every second
                u.tacTimer = 1.f;
                if (dist < u.guardBest - 0.5f) u.guardBest = dist, u.step = 1;
                else u.step = 2;
            }
            if (u.step != 2 && dist > 6.f) {
                if (std::fabs(err) < 1.2f) in.throttle = 1, in.run = dist > 40.f;
            } else {
                in.turn = 0;
                if (dist > 12.f && u.step == 2 && u.tacTimer > 0.95f) u.guardBest = dist + 20.f;  // try again later
            }
        } else if (u.mobile && u.leader >= 0 && u.order == OrderDefend && positionOf(u.orderObj, tp)) {
            // SquadMateDefend (FUN_100adc20): radius r (32 m). Far (≥0.6 r): MOVETO the charge at speed 5 until inside
            // 0.4 r. Close and the charge standing still: wander to a random point within r of it, STAND, wait 3 s.
            const float r = 32.f;
            float dist = length(flat(tp) - flat(me));
            Vec3 tv = (tp - u.leaderLast) * (1.f / std::max(0.016f, dt));
            u.leaderLast = tp;
            if (u.ord.kind == 2 && u.step == 3 && dist < 0.4f * r) u.ord = Unit::Order{};
            if (u.ord.kind == 0) {
                std::uniform_real_distribution<float> R(0.f, 1.f);
                if (dist >= 0.6f * r) u.ord.kind = 2, u.ord.pos = tp, u.ord.speed = 5.f, u.step = 3;
                else if (dist <= r - 10.f && length(tv) <= 0.1f) {
                    if (u.step == 4) u.ord.kind = 99, u.ord.timer = 3.f, u.step = 0;  // STAND + wait
                    else {
                        float a2 = R(rng_) * 6.2832f, rr = r * R(rng_);
                        u.ord.kind = 2, u.ord.pos = tp + Vec3(std::sin(a2), 0, std::cos(a2)) * rr, u.ord.speed = 3.f, u.step = 4;
                    }
                }
            }
            if (u.ord.kind == 2) {
                if (u.step == 3) u.ord.pos = tp;
                float e2, d2 = steerTo(u.ord.pos, e2);
                if (d2 < 4.f) u.ord = Unit::Order{};
                else if (std::fabs(e2) < 1.2f) in.throttle = 1, in.run = u.ord.speed >= 4.f && d2 > 25.f;
            } else if (u.ord.kind == 99) {
                float e2;
                steerTo(tp, e2), in.turn = 0;
                if ((u.ord.timer -= dt) <= 0) u.ord = Unit::Order{};
            }
        } else if (u.mobile && u.leader >= 0 && positionOf(u.leader, leaderPos) && combat_->alive(u.leader)) {
            // formation slot behind the leader, rotated by the leader's heading (models face -Z)
            float ly = u.leader == player_->object() ? player_->yaw() : u.body.yaw();
            if (const Unit* l = find(u.leader)) ly = l->body.yaw();
            Vec3 right(-std::cos(ly), 0, std::sin(ly)), back(std::sin(ly), 0, std::cos(ly));
            Vec3 want = leaderPos + right * u.slot.x + back * u.slot.z;
            if (const Surface* s = scene_->surfaceAt(want.x, want.z); s && s->damage > 0) {
                // the slot is on lava: the nearest safe point between it and the leader
                for (float f = 0.25f; f <= 1.f; f += 0.25f) {
                    Vec3 q = want + (leaderPos - want) * f;
                    const Surface* s2 = scene_->surfaceAt(q.x, q.z);
                    if (!s2 || s2->damage <= 0) { want = q; break; }
                }
            }
            // SquadMateFollow (FUN_100ad160, parameters 32 / 4 / 16 m): set off for the slot once it is more than 16 m
            // away (or 4 m with the leader on the move), settle inside 4 m, run to catch up past 32 m
            float lspeed = length(flat(leaderPos) - flat(u.leaderLast)) / std::max(0.016f, dt);
            u.leaderLast = leaderPos;
            float err, dist = steerTo(want, err);
            if (!u.following && (dist > 16.f || (lspeed > 1.f && dist > 4.f))) u.following = true;
            if (u.following && dist < 4.f) u.following = false;
            if (u.following) {
                if (std::fabs(err) < 1.2f) in.throttle = 1, in.run = dist >= 32.f || (lspeed > 8.f && dist > 8.f);
            } else {  // in place: face the way the leader faces
                float e2 = wrap(ly - u.body.yaw());
                in.turn = std::fabs(e2) > 0.1f ? std::max(-1.f, std::min(1.f, e2 * 2.f)) : 0.f;
            }
        } else if (u.mobile && playerAlive && teamOf(player_->object()) != u.team &&
                   (u.nonCombatTactic.find("hunt") != std::string::npos || alert >= 5)) {
            // HuntUser (FUN_100ab880) / full alert: MOVETO where the user is, re-planned only once the user has moved
            // more than 128 m from that spot; HuntTarget (FUN_100aba50) aims 16 m to a random side, re-plans past 64 m
            Vec3 pp = player_->position();
            bool huntTarget = u.nonCombatTactic.find("hunttarget") != std::string::npos;
            float limit = huntTarget ? 64.f : 128.f;
            if (u.ord.kind != 2 || length(u.goal - pp) > limit) {
                std::uniform_real_distribution<float> R(-1.f, 1.f);
                u.goal = pp;
                u.ord = Unit::Order{}, u.ord.kind = 2, u.ord.pos = pp + (huntTarget ? Vec3(R(rng_), 0, R(rng_)) * 16.f : Vec3());
            }
            float err, dist = steerTo(u.ord.pos, err);
            if (dist < 20.f) u.ord.kind = 0;  // there: look again from here
            else if (std::fabs(err) < 1.2f && length(pp - me) > 60.f) in.throttle = 1, in.run = dist > 200.f;
        } else if (u.mobile && u.leader < 0 && u.target < 0 &&
                   (u.nonCombatTactic.find("patrol") != std::string::npos || u.nonCombatTactic.find("freerange") != std::string::npos)) {
            // GearPatrol1 / DefaultPatrol / VehiclePatrol1 / FreeRange1 (FUN_100abee0, FUN_100af590, FUN_100af2f0):
            // walk to a random spot round home (64 m; vehicles further), stand, wait a few seconds, again
            std::uniform_real_distribution<float> R(0.f, 1.f);
            bool vehicle = u.type == 0x4e || u.type == 0x4f;
            if (u.ord.kind == 0) {
                if (u.step == 1) u.ord.kind = 99, u.ord.timer = 2.f + R(rng_) * 4.f, u.step = 0;
                else {
                    float span = u.freeRadius > 0 && u.nonCombatTactic.find("freerange") != std::string::npos ? u.freeRadius
                               : vehicle ? 160.f : 64.f;
                    float r = span * (0.3f + 0.7f * R(rng_)), a2 = R(rng_) * 6.2832f;
                    u.ord.kind = 2, u.ord.pos = u.home + Vec3(std::sin(a2), 0, std::cos(a2)) * r, u.ord.speed = 3.f, u.step = 1;
                }
            }
            if (u.ord.kind == 2) {
                float e2, d2 = steerTo(u.ord.pos, e2);
                if (d2 < 6.f) u.ord = Unit::Order{};
                else if (std::fabs(e2) < 1.2f) in.throttle = 1;
            } else if (u.ord.kind == 99 && (u.ord.timer -= dt) <= 0) {
                u.ord = Unit::Order{};
            }
        } else if (u.target < 0 && u.attacker >= 0 && u.nonCombatTactic.find("holdground") != std::string::npos &&
                   positionOf(u.attacker, tp)) {
            float err;  // DefaultHoldGround (FUN_100afb50): stand and face where the fire came from
            steerTo(tp, err);
        } else if (u.lookAt >= 0 && positionOf(u.lookAt, tp)) {
            float err;
            steerTo(tp, err);
            if (std::fabs(err) < 0.05f) in.turn = 0;
        }
        // standing on damaging ground (lava): walk to the nearest safe spot first
        if (u.mobile && u.type == 0x48) {
            auto hot = [&](const Vec3& p) { const Surface* s = scene_->surfaceAt(p.x, p.z); return s && s->damage > 0; };
            if (hot(me)) {
                Vec3 best;
                float bd = 1e9f;
                for (float r : {12.f, 25.f, 40.f, 60.f, 90.f}) {
                    for (int k = 0; k < 12; k++) {
                        float a2 = float(k) * 0.5236f;
                        Vec3 p = me + Vec3(std::sin(a2), 0, std::cos(a2)) * r;
                        if (!hot(p) && r < bd) bd = r, best = p;
                    }
                    if (bd < 1e8f) break;
                }
                if (getenv("HG2_DEBUG") && int(u.clock * 2) != int((u.clock - dt) * 2))
                    SDL_Log("ai %d on lava at %.0f,%.0f: escape %s %.0f m", u.object, me.x, me.z, bd < 1e8f ? "to" : "none", bd);
                if (bd < 1e8f) {
                    float e3;
                    steerTo(best, e3);
                    in.throttle = std::fabs(e3) < 1.4f ? 1.f : 0.3f, in.run = true, in.strafe = 0;
                }
            }
        }
        // MOVETO progress watchdog (FUN_100bf300): the follower keeps the nearest it has been to its goal and a 5 s
        // timer, reset whenever it gets nearer; when the timer runs out the move fails (state 7) and the tactic picks
        // again. The port first tries a planned route, then gives the order up.
        if (u.ord.kind == 2 && !u.braked && u.mobile) {
            float d = length(flat(u.ord.pos) - flat(me));
            if (d < u.ord.best - 0.5f) u.ord.best = d, u.ord.patience = 5.f;
            else if ((u.ord.patience -= dt) <= 0) {
                if (!u.ord.rerouted) u.ord.rerouted = true, u.ord.patience = 5.f, u.wantRoute = true, u.lastSteer = u.ord.pos;
                else u.ord = Unit::Order{}, u.detour.clear();
            }
        }
        // route round what's in the way: when stuck, or (every 1.5 s) when the straight line ahead is blocked
        if (u.mobile && !u.braked && in.throttle > 0.5f && u.detour.empty() && length(flat(u.lastSteer) - flat(me)) > 20.f) {
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
            if (getenv("HG2_TACDBG"))
                SDL_Log("ai %d route to %.0f,%.0f: %zu waypoints", u.object, u.lastSteer.x, u.lastSteer.z, u.detour.size());
        }
        // stuck against something while trying to drive: back off and turn for a moment
        if (u.mobile && u.type == 0x48) {
            if (u.unstick > 0) {
                u.unstick -= dt;
                in.throttle = -0.7f, in.turn = (u.object & 1) ? 1.f : -1.f, in.strafe = 0, in.run = false;
            } else if (std::fabs(in.throttle) > 0.5f && !u.braked) {
                if ((u.stuckClock += dt) > 2.f) {
                    Vec3 dd = me - u.stuckFrom;
                    if (length(Vec3(dd.x, 0, dd.z)) < 1.f) u.unstick = 1.2f, u.wantRoute = true;
                    u.stuckClock = 0, u.stuckFrom = me;
                }
            } else {
                u.stuckClock = 0, u.stuckFrom = me;
            }
        }
        // gears skate on long runs out of a fight (the original's SMS travel) and kneel to shoot when holding
        // ground; scripted stances are left alone
        if (u.type == 0x48 && u.mobile && !u.scriptStance && u.body.canWalk()) {
            // (not on scripted paths: escorts and patrols keep the pace their scripts expect)
            bool travelling = !getenv("HG2_NOSKATE") && u.target < 0 && u.path.empty() && u.drive == 0 && in.throttle > 0.5f && in.run && std::fabs(in.turn) < 0.5f;
            if (travelling != u.autoSkates) u.autoSkates = travelling, u.body.setSkates(travelling);
            bool hold = u.target >= 0 && std::fabs(in.throttle) < 0.1f && std::fabs(in.strafe) < 0.1f &&
                        (u.order == OrderHold || u.combatTactic.find("holdground") != std::string::npos || u.combatTactic == "standandshoot");
            if (hold != u.autoKneel) u.autoKneel = hold, u.body.setPosture(hold ? 2 : 1);
        }
        in.throttle *= std::min(1.f, u.speedFrac * 2.f);
        if (u.braked) in.throttle = 0, in.strafe = 0, in.run = false;
        if (!u.mobile) {  // turrets only rotate
            u.body.setYaw(u.body.yaw() + in.turn * 1.2f * dt);
            in = PlayerInput{};
        }
        if (getenv("HG2_UNITDBG") && u.object == atoi(getenv("HG2_UNITDBG")) && int(u.clock * 2) != int((u.clock - dt) * 2))
            SDL_Log("unit %d thr %.2f run %d turn %.2f braked %d speedFrac %.2f bodySpeed %.2f walk %d ord %d drive %d",
                    u.object, in.throttle, in.run, in.turn, u.braked, u.speedFrac, u.body.speed(), u.body.canWalk(), u.ord.kind, u.drive);
        u.body.update(dt, in);
        combat_->setPosture(u.object, u.body.postureCode());
    }
}


// ---- route planning. The original's MOVETO asks a pathfinder for waypoints (FUN_100bf300, state at +200); its
// search isn't decoded, so this is the port's own: an A* over 6 m cells round the straight line, with steep steps,
// solid objects and lava (heavily penalised) as the obstacles, then string-pulled to a few waypoints.
namespace {
constexpr float kCell = 6.f;
}
bool AiSystem::lineBlocked(const Vec3& from, const Vec3& to, int self) const {
    Vec3 d = flat(to) - flat(from);
    float len = length(d);
    if (len < 1.f) return false;
    int n = int(len / 4.f) + 1;
    float hPrev = scene_->groundHeight(from.x, from.z);
    for (int i = 1; i <= n; ++i) {
        Vec3 p = from + d * (float(i) / float(n));
        float h = scene_->groundHeight(p.x, p.z);
        if (std::fabs(h - hPrev) > 0.9f * (len / float(n))) return true;  // steeper than ~42°
        hPrev = h;
        Vec3 q(p.x, h, p.z);
        if (scene_->collide(q, 2.5f, 8.f, self)) return true;
        const Surface* s = scene_->surfaceAt(p.x, p.z);
        if (s && s->damage > 0) return true;
    }
    return false;
}
bool AiSystem::planRoute(const Vec3& from, const Vec3& to, int self, std::vector<Vec3>& out) const {
    out.clear();
    float x0 = std::min(from.x, to.x) - 96.f, z0 = std::min(from.z, to.z) - 96.f;
    int W = std::min(160, int((std::max(from.x, to.x) + 96.f - x0) / kCell) + 1);
    int H = std::min(160, int((std::max(from.z, to.z) + 96.f - z0) / kCell) + 1);
    auto cellOf = [&](const Vec3& p, int& cx, int& cz) {
        cx = std::clamp(int((p.x - x0) / kCell), 0, W - 1), cz = std::clamp(int((p.z - z0) / kCell), 0, H - 1);
    };
    int sx, sz, gx, gz;
    cellOf(from, sx, sz), cellOf(to, gx, gz);
    // per cell: -1 unknown, 0 free, 1 solid, 2 lava; heights cached
    std::vector<signed char> kind(size_t(W) * H, -1);
    std::vector<float> hgt(size_t(W) * H, 0.f);
    auto centre = [&](int cx, int cz) { return Vec3(x0 + (cx + 0.5f) * kCell, 0, z0 + (cz + 0.5f) * kCell); };
    auto classify = [&](int cx, int cz) {
        size_t i = size_t(cz) * W + cx;
        if (kind[i] < 0) {
            Vec3 c = centre(cx, cz);
            float h = scene_->groundHeight(c.x, c.z);
            hgt[i] = h;
            Vec3 q(c.x, h, c.z);
            const Surface* s = scene_->surfaceAt(c.x, c.z);
            kind[i] = scene_->collide(q, 2.5f, 8.f, self) ? 1 : (s && s->damage > 0) ? 2 : 0;
        }
        return kind[i];
    };
    std::vector<float> g(size_t(W) * H, 1e30f);
    std::vector<int> parent(size_t(W) * H, -1);
    using QE = std::pair<float, int>;
    std::priority_queue<QE, std::vector<QE>, std::greater<QE>> open;
    int start = sz * W + sx, goal = gz * W + gx;
    classify(sx, sz);
    g[start] = 0;
    open.push({0.f, start});
    int expanded = 0, best = start;
    float bestH = 1e30f;
    while (!open.empty() && expanded < 6000) {
        auto [f, cur] = open.top();
        open.pop();
        int cx = cur % W, cz = cur / W;
        float hc = float(std::hypot(gx - cx, gz - cz));
        if (hc < bestH) bestH = hc, best = cur;
        if (cur == goal) break;
        if (f > g[cur] + hc * 1.0001f + 1e-3f) continue;  // stale
        ++expanded;
        for (int dz = -1; dz <= 1; ++dz)
            for (int dx = -1; dx <= 1; ++dx) {
                if (!dx && !dz) continue;
                int nx = cx + dx, nz = cz + dz;
                if (nx < 0 || nz < 0 || nx >= W || nz >= H) continue;
                int k = classify(nx, nz);
                if (k == 1 && !(nx == gx && nz == gz)) continue;
                float step = (dx && dz) ? 1.4142f : 1.f;
                if (std::fabs(hgt[size_t(nz) * W + nx] - hgt[size_t(cur)]) > 0.9f * kCell * step) continue;  // too steep
                float c = g[cur] + step * (k == 2 ? 12.f : 1.f);
                int ni = nz * W + nx;
                if (c < g[ni]) g[ni] = c, parent[ni] = cur, open.push({c + float(std::hypot(gx - nx, gz - nz)), ni});
            }
    }
    if (best == start) return false;
    std::vector<int> cells;
    for (int c = best; c >= 0; c = parent[c]) cells.push_back(c);
    std::reverse(cells.begin(), cells.end());
    // string-pull: keep a cell only where the straight line from the last kept point stops being clear
    Vec3 last = from;
    for (size_t i = 1; i < cells.size(); ++i) {
        Vec3 c = centre(cells[i] % W, cells[i] / W);
        bool end = i + 1 == cells.size();
        Vec3 nxt = end ? c : centre(cells[i + 1] % W, cells[i + 1] / W);
        if (end || lineBlocked(last, nxt, self)) out.push_back(c), last = c;
    }
    if (best == goal) out.back() = Vec3(to.x, 0, to.z);
    return !out.empty();
}
}  // namespace hg2
