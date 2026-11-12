#include "combat.h"

#include "particles.h"

#include "../audio/audio.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace hg2 {

namespace {
// Unit box from z = 0..1 (x, y = -0.5..0.5), flat-coloured, unlit.
std::vector<Vertex> box(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    const float p[8][3] = {{-.5f, -.5f, 0}, {.5f, -.5f, 0}, {.5f, .5f, 0}, {-.5f, .5f, 0},
                           {-.5f, -.5f, 1}, {.5f, -.5f, 1}, {.5f, .5f, 1}, {-.5f, .5f, 1}};
    const int f[6][4] = {{0, 1, 2, 3}, {5, 4, 7, 6}, {4, 0, 3, 7}, {1, 5, 6, 2}, {3, 2, 6, 7}, {4, 5, 1, 0}};
    std::vector<Vertex> v;
    for (auto& q : f)
        for (int k : {0, 1, 2, 0, 2, 3}) {
            Vertex x{};
            for (int c = 0; c < 3; c++) x.pos[c] = p[q[k]][c];
            x.normal[1] = 1;
            x.color[0] = r, x.color[1] = g, x.color[2] = b, x.color[3] = a;
            v.push_back(x);
        }
    return v;
}

// Orient the unit box along `dir` with length `len` and thickness `w`, starting at `from`.
Mat4 along(const Vec3& from, const Vec3& dir, float len, float w) {
    Vec3 z = normalize(dir);
    Vec3 up = std::fabs(z.y) > 0.95f ? Vec3(1, 0, 0) : Vec3(0, 1, 0);
    Vec3 x = normalize(cross(up, z)), y = cross(z, x);
    Mat4 m = Mat4::identity();
    Vec3 rows[3] = {x * w, y * w, z * len};
    for (int r = 0; r < 3; r++) m.m[r][0] = rows[r].x, m.m[r][1] = rows[r].y, m.m[r][2] = rows[r].z;
    m.m[3][0] = from.x, m.m[3][1] = from.y, m.m[3][2] = from.z;
    return m;
}

bool isUnitClass(int type) {
    // Bipeded, Turret, Jeep, HoverTank, Infantery, Mount, ChickenWalker, Plane objects
    return type == 0x48 || type == 0x4b || type == 0x4e || type == 0x4f || type == 0x50 || type == 0x51 ||
           type == 0x52 || type == 0x55;
}
}  // namespace

bool Combat::init(const FileSystem& fs, Scene& scene, const GearDef& def, int playerObject) {
    scene_ = &scene;
    playerObject_ = playerObject;
    damage.assign(scene.mission().objects.size(), 0.f);
    Bytes b;
    if (!fs.read("MstrWpns.bin", b) || !loadWeapons(b, weapons_)) {
        SDL_Log("MstrWpns.bin not found");
        return false;
    }
    addUnit(playerObject, def);
    // structure points for everything else that has a definition (buildings 200..1000, 99999 = scenery)
    const Mission& m = scene.mission();
    for (size_t i = 0; i < m.objects.size(); i++) {
        const MissionObject& o = m.objects[i];
        if (o.defIndex < 0 || size_t(o.defIndex) >= m.defs.size() || int(i) == playerObject) continue;
        GearDef g;
        if (parseGearDef(m.defs[size_t(o.defIndex)].data, m.defs[size_t(o.defIndex)].size, g) && g.hitPoints > 0 &&
            g.hitPoints < 99999)
            structure_[int(i)] = float(g.hitPoints);
    }
    Renderer& r = scene.renderer();
    auto mk = [&](uint8_t cr, uint8_t cg, uint8_t cb, uint8_t ca) {
        auto v = box(cr, cg, cb, ca);
        return r.createMesh(v, {{r.whiteTexture(), 0, uint32_t(v.size()), ca < 255, false}});
    };
    tracer_ = mk(255, 220, 120, 255);
    beam_ = mk(255, 60, 40, 200);
    flash_ = mk(255, 170, 60, 160);
    std::string names;
    for (auto& l : units_[playerObject].loadout) names += l.def->name + " ";
    SDL_Log("player weapons: %s", names.c_str());
    return true;
}

// Loadout from the gear's hardpoints (shell variants override this once the shell is ported).
std::vector<Loadout> Combat::buildLoadout(const GearDef& def) {
    std::vector<Loadout> out;
    for (auto& hp : def.hardpoints) {
        const char* nm = weaponForHardpointKeyword(hp.weapon);
        if (!nm) continue;
        for (auto& w : weapons_)
            if (w.name == nm) {
                out.push_back({&w, hp.slot, w.ammunition > 0 ? w.ammunition : -1, 0});
                break;
            }
    }
    if (out.empty())
        for (auto& w : weapons_)
            if (w.name == "LAC") out.push_back({&w, 1, w.ammunition, 0});
    return out;
}

void Combat::setLoadout(int object, const std::vector<std::pair<int, int>>& weapons) {
    auto it = units_.find(object);
    if (it == units_.end()) return;
    std::vector<Loadout> out;
    for (auto& [slot, idx] : weapons)
        if (idx >= 0 && size_t(idx) < weapons_.size()) {
            const WeaponDef& w = weapons_[size_t(idx)];
            out.push_back({&w, slot, w.ammunition > 0 ? w.ammunition : -1, 0});
        }
    if (out.empty()) return;
    // ranged weapons first: melee weapons share a hand with a gun and are the last resort
    std::stable_partition(out.begin(), out.end(), [](const Loadout& l) { return l.def->type != 2; });
    it->second.loadout = out;
    it->second.broken.assign(out.size(), false);
    it->second.selected = 0;
}

bool isUnitObjectType(int type) { return isUnitClass(type); }

void Combat::setRemoteHealth(int object, bool alive, float fraction) {
    auto it = units_.find(object);
    if (it == units_.end()) return;
    Unit& u = it->second;
    if (!alive && u.hp > 0) {
        u.hp = 0;
        Vec3 p;
        if (scene_->objectPosition(object, p)) destroyObject(object, p);
    } else if (alive) {
        u.hp = std::max(1.f, u.maxHp * std::clamp(fraction, 0.f, 1.f));
    }
}

void Combat::revive(int object, const GearDef& def) {
    auto it = units_.find(object);
    if (it == units_.end()) return;
    Unit keep = it->second;
    addUnit(object, def);
    Unit& u = units_[object];
    u.loadout = keep.loadout, u.broken.assign(u.loadout.size(), false), u.remote = keep.remote;
    for (auto& l : u.loadout)
        if (l.def && l.def->ammunition > 0) l.ammo = l.def->ammunition;
    if (keep.maxHp > 0 && u.armoured) {  // keep shell stats (setStats)
        float k = keep.maxHp / std::max(1.f, u.maxHp);
        u.hp = u.maxHp = keep.maxHp;
        for (auto& p : u.parts) p.hp *= k, p.maxHp *= k;
    }
    u.fireControl = keep.fireControl;
}

void Combat::setSystems(int object, int sensors, int maneuver, uint32_t perks, uint32_t flaws) {
    auto it = units_.find(object);
    if (it == units_.end()) return;
    it->second.sensors = sensors, it->second.maneuverStat = maneuver, it->second.perks = perks, it->second.flaws = flaws;
}

float Combat::aimSpread(int shooter, int target) const {
    float k = 1.f;
    int jam = ecm(target) - eccm(shooter);
    if (jam > 0) k *= 1.f + 0.15f * float(jam);
    k *= 1.f + 0.25f * float(field(shooter, 1, 17, 2));               // Defective Fire Control
    k *= std::max(0.6f, 1.f + 0.08f * float(maneuverRating(target)));  // nimble gears are harder to hit
    if (sniper(shooter)) k *= 0.75f;                                     // Sniper Systems
    if (target == designated_ && shooter >= 0 && size_t(shooter) < scene_->mission().objects.size() &&
        scene_->mission().objects[size_t(shooter)].team == designatorTeam_)
        k *= 0.7f;                                                       // a designated target
    return k;
}

float Combat::concealment(int object) const {
    auto it = units_.find(object);
    if (it == units_.end()) return 0.15f;
    const Unit& u = it->second;
    float base = u.posture == 0 ? 0.4f : u.posture == 2 ? 0.25f : 0.15f;
    float c = base + 0.05f * float(u.flaws >> 10 & 7) - 0.15f * float(u.flaws >> 25 & 1);
    c = std::clamp(c, 0.f, 1.f);
    if (clock_ - u.firedAt < 2.f) c *= 0.5f;  // muzzle flashes give a gear away
    return c;
}

float Combat::detectFactor(int viewer, int target) const {
    float k = std::max(0.4f, 1.f + 0.1f * float(sensorRating(viewer)));
    // concealment against a standing gear's 0.15; ECCM sees through some of the stealth
    float c = concealment(target) - 0.025f * float(eccm(viewer));
    k *= std::max(0.2f, (1.f - std::max(0.f, c)) / 0.85f);
    if (field(viewer, 1, 15, 2)) k *= 1.f - 0.15f * float(field(viewer, 1, 15, 2));  // Defective Active Sensors
    return k;
}

void Combat::setStats(int object, int armor, int fireControl) {
    auto it = units_.find(object);
    if (it == units_.end() || !it->second.armoured || armor <= 0) return;
    Unit& u = it->second;
    float k = float(armor) * 10.f / std::max(1.f, u.maxHp);
    u.hp = u.maxHp = float(armor) * 10.f;
    for (auto& p : u.parts) p.hp *= k, p.maxHp *= k;
    u.fireControl = fireControl;
}

void Combat::addUnit(int object, const GearDef& def) {
    Unit u;
    u.loadout = buildLoadout(def);
    u.broken.assign(u.loadout.size(), false);
    u.armoured = def.armor > 0;
    // FUN_1004bcf0 stores Armor x 10; things without it use their structure points (0xad)
    float hp = u.armoured ? float(def.armor) * 10.f : float(def.hitPoints > 0 ? def.hitPoints : 50);
    u.hp = u.maxHp = hp;
    u.fireControl = def.fireControl;
    u.sensors = def.sensors, u.maneuverStat = def.maneuverability, u.perks = def.perks, u.flaws = def.flaws;
    u.damageTexture = def.damageTexture;
    structure_.erase(object);
    for (auto& d : def.damageParts) {
        Part p;
        p.frame = upper(d.frame), p.system = d.system;
        std::string f = p.frame;
        // a part's share of the unit: its armour value (0..63) as a fraction of the gear's hit points; limbs
        // and sensors get half that so they can be shot off before the gear falls
        bool torso = f.compare(0, 2, "BD") == 0 && f != "BDC";
        p.maxHp = p.hp = std::max(1.f, hp * float(d.armour) / 63.f * (torso ? 1.f : 0.5f));
        if (f == "BDC") p.region = 'H';
        else if (f.compare(0, 2, "RA") == 0) p.region = 'A';
        else if (f.compare(0, 2, "LA") == 0) p.region = 'a';
        else if (f.compare(0, 2, "RL") == 0) p.region = 'L';
        else if (f.compare(0, 2, "LL") == 0) p.region = 'l';
        else p.region = 'T';
        u.parts.push_back(p);
    }
    units_[object] = std::move(u);
}

float Combat::regionHealth(int object, char region) const {
    auto it = units_.find(object);
    if (it == units_.end()) return 1.f;
    float h = 1.f;
    bool any = false;
    for (auto& p : it->second.parts)
        if (p.region == region) h = std::min(h, p.gone ? 0.f : p.hp / p.maxHp), any = true;
    return any ? std::max(0.f, h) : 1.f;
}

float Combat::mobility(int object) const {
    auto it = units_.find(object);
    if (it == units_.end()) return 1.f;
    float m = wading(object, scene_->mission().waterLevel);
    if (space_ && !zeroG(object)) m *= 0.75f;  // footing in low gravity, unless built for Zero-G Movement
    for (char leg : {'L', 'l'}) {
        bool lost = false;
        for (auto& p : it->second.parts)
            if (p.region == leg && p.gone) lost = true;
        if (lost) m -= 0.35f;
    }
    m *= 0.4f + 0.15f * float(std::max(0, it->second.mobility));  // critical hits on the drive system
    return std::max(0.2f, std::min(1.f, m));
}

float Combat::wading(int object, float waterLevel) const {
    Vec3 p;
    if (waterLevel <= 0 || !scene_->objectPosition(object, p)) return 1.f;
    float depth = waterLevel - p.y;
    if (depth <= 1.f) return 1.f;
    return amphibious(object) ? 1.f : std::max(0.45f, 1.f - 0.08f * depth);
}

float Combat::manoeuvre(int object) const {
    auto it = units_.find(object);
    if (it == units_.end()) return 1.f;
    // turn rate scales with the Maneuverability rating, 0.6 + 0.05 x rating (FUN_1003aaf0); 1 at rating 0
    float rating = std::max(0.5f, (0.6f + 0.05f * float(it->second.maneuverStat)) / 0.6f);
    return std::max(0.2f, float(it->second.manoeuvre + 5) / 5.f) * rating;
}

bool Combat::weaponBroken(int object, int index) const {
    auto it = units_.find(object);
    return it != units_.end() && index >= 0 && size_t(index) < it->second.broken.size() && it->second.broken[size_t(index)];
}

bool Combat::sensorsOkRaw(int object) const {
    auto it = units_.find(object);
    if (it == units_.end()) return true;
    for (auto& p : it->second.parts)
        if (p.system == 3 && p.gone) return false;
    return true;
}

bool Combat::sensorsOk(int object) const {
    auto it = units_.find(object);
    if (it == units_.end()) return true;
    if (it->second.perks >> 5 & 1) return true;  // Backup Sensors
    for (auto& p : it->second.parts)
        if (p.system == 3 && p.gone) return false;
    return true;
}

bool Combat::muzzle(int shooter, Vec3& out) const {
    auto it = units_.find(shooter);
    if (it == units_.end() || it->second.loadout.empty()) return false;
    int slot = it->second.loadout[size_t(it->second.selected)].slot;
    return scene_->frameWorld(shooter, "hp" + std::to_string(slot), out);
}

void Combat::partDestroyed(int object, Unit& u, Part& part, const Vec3& at) {
    part.gone = true, part.hp = 0;
    if (particles) particles->spawn("DAMAGE_SPARK", at, 1.f);
    if (audio) audio->play("explsm01.wav", 0.8f, &at, 500.f);
    const char* what = part.region == 'H' ? "SENSORS DESTROYED" : part.region == 'A' ? "RIGHT ARM DESTROYED"
                     : part.region == 'a' ? "LEFT ARM DESTROYED" : part.region == 'L' ? "RIGHT LEG DAMAGED"
                     : part.region == 'l' ? "LEFT LEG DAMAGED" : "TORSO BREACHED";
    if (object == playerObject_) log.push_back(what);
    if (getenv("HG2_DEBUG")) SDL_Log("unit %d: %s (%s)", object, what, part.frame.c_str());
    if (part.region == 'A' || part.region == 'a') {
        // the arm comes off with whatever is mounted on it
        scene_->hideFrame(object, part.frame);
        auto& lo = u.loadout;
        for (size_t k = 0; k < lo.size();)
            if (scene_->frameUnder(object, "hp" + std::to_string(lo[k].slot), part.frame))
                lo.erase(lo.begin() + long(k)), u.broken.erase(u.broken.begin() + long(k));
            else k++;
        if (u.selected >= int(lo.size())) u.selected = 0;
    }
    // structural torso parts carry the gear
    if (part.system == 2 && part.region == 'T') u.hp = std::min(u.hp, 0.f);
}

float Combat::health(int object) const {
    auto it = units_.find(object);
    return it == units_.end() || it->second.maxHp <= 0 ? 1.f : std::max(0.f, it->second.hp / it->second.maxHp);
}

float Combat::range(int shooter) const {
    auto it = units_.find(shooter);
    if (it == units_.end() || it->second.loadout.empty()) return 0;
    return it->second.loadout[size_t(it->second.selected)].def->maxRange;
}

float Combat::chooseWeapon(int shooter, float dist) {
    auto it = units_.find(shooter);
    if (it == units_.end() || it->second.loadout.empty()) return 0;
    Unit& u = it->second;
    int best = -1;
    float bestScore = -1;
    for (size_t i = 0; i < u.loadout.size(); i++) {
        const Loadout& l = u.loadout[i];
        if (!l.def || l.ammo == 0 || (i < u.broken.size() && u.broken[i])) continue;
        bool melee = l.def->type == 2;
        float reach = melee ? 6.f : l.def->maxRange;
        if (melee && dist > 8.f) continue;
        float score = l.def->damage / std::max(0.05f, l.def->nextRoundTime);  // damage per second
        if (dist > reach) score *= 0.05f;                                    // keep as a fallback only
        if (melee && dist <= 8.f) score *= 3.f;
        if (score > bestScore) bestScore = score, best = int(i);
    }
    if (best < 0) return 0;
    u.selected = best;
    return u.loadout[size_t(best)].def->type == 2 ? 6.f : u.loadout[size_t(best)].def->maxRange;
}

bool Combat::hasMelee(int shooter) const {
    auto it = units_.find(shooter);
    if (it == units_.end()) return false;
    for (auto& l : it->second.loadout)
        if (l.def && l.def->type == 2) return true;
    return false;
}

float Combat::longestRange(int shooter) const {
    auto it = units_.find(shooter);
    if (it == units_.end()) return 0;
    float r = 0;
    for (size_t i = 0; i < it->second.loadout.size(); i++) {
        const Loadout& l = it->second.loadout[i];
        if (l.def && l.ammo != 0 && !(i < it->second.broken.size() && it->second.broken[i]) && l.def->type != 2) r = std::max(r, l.def->maxRange);
    }
    return r;
}

void Combat::selectWeapon(int i) {
    auto& u = units_[playerObject_];
    if (i >= 0 && i < int(u.loadout.size())) u.selected = i;
}
void Combat::nextWeapon() {
    auto& u = units_[playerObject_];
    if (!u.loadout.empty()) u.selected = (u.selected + 1) % int(u.loadout.size());
}

bool Combat::fire(int shooter, const Vec3& muzzle, const Vec3& aim) {
    auto it = units_.find(shooter);
    if (it == units_.end() || it->second.loadout.empty() || it->second.hp <= 0) return false;
    Loadout& l = it->second.loadout[size_t(it->second.selected)];
    if (l.cooldown > 0 || l.ammo == 0 || weaponBroken(shooter, it->second.selected)) return false;
    const WeaponDef& w = *l.def;
    l.cooldown = std::max(0.03f, w.nextRoundTime);
    it->second.firedAt = clock_;
    if (l.ammo > 0) l.ammo--;
    shotsBy[shooter]++;
    Vec3 dir = normalize(aim - muzzle);
    if (audio) audio->play(w.fireSound, shooter == playerObject_ ? 0.6f : 0.8f, &muzzle, 500.f);
    if (particles && w.type != 2) particles->spawn(w.beam() ? "MUZZLE_FLASH2" : "MUZZLE_FLASH", muzzle, 0.3f, dir, 0.3f);
    float range = std::max(20.f, w.maxRange);
    if (w.type == 2) range = 6.f;  // melee
    if (w.beam()) {  // hitscan
        Vec3 end = muzzle + dir * range;
        float tg = scene_->terrainRay(muzzle, end), to;
        int obj = scene_->raycast(muzzle, end, shooter, to);
        float t = std::min(tg, to);
        if (t <= 1.f) {
            end = muzzle + (end - muzzle) * t;
            impact(end, w, to <= tg ? obj : -1, shooter);
        }
        shots_.push_back({muzzle, end - muzzle, 0.08f, &w, true, shooter});
        return true;
    }
    float speed = std::max(60.f, w.velocity);
    Shot sh{muzzle, dir * speed, range / speed, &w, false, shooter};
    static const char* guided[] = {"AAM", "AGM", "ATM", "HATM"};
    for (const char* g : guided)
        if (w.name == g) {
            auto t = aimTarget_.find(shooter);
            if (t != aimTarget_.end() && t->second >= 0 && alive(t->second)) {
                sh.target = t->second;
                // the target's ECM tries to break the lock; the shooter's ECCM holds it (ECCM x 100 against a
                // roll, FUN_101117f0)
                int e = ecm(sh.target);
                sh.jammed = e > 0 && rand() % 1000 >= eccm(shooter) * 100 && rand() % 100 < e * 20;
                sh.life *= 1.3f;
            }
            break;
        }
    shots_.push_back(sh);
    return true;
}

void Combat::impact(const Vec3& p, const WeaponDef& w, int object, int shooter, float scale) {
    if (getenv("HG2_DEBUG")) fprintf(stderr, "impact %s obj %d by %d at %.0f %.0f %.0f\n", w.name.c_str(), object, shooter, p.x, p.y, p.z);
    // impact effect: metal hits on objects (beam / missile / mortar / ballistic, by weight), dirt on the ground
    bool fx = false;
    if (particles) {
        char sz = w.damage >= 40.f ? 'L' : w.damage >= 12.f ? 'M' : 'S';
        std::string kind = w.beam() ? "MTPA" : (w.type == 3 || w.type == 7 || w.mortarAngle > 0) ? "MTMO" : w.velocity < 300 ? "MTMI" : "MTBA";
        float dens = w.damage >= 40.f ? 0.5f : 0.2f;
        if (object >= 0) fx = particles->spawn(kind + sz, p, 0.6f, Vec3(0, 1, 0), dens);
        else fx = particles->spawn(w.damage >= 30.f || w.collateralRadius > 3.f ? "DIRT BLAST" : "SPARKDUST", p, 0.8f, Vec3(0, 1, 0), dens);
    }
    if (!fx) flashes_.push_back({p, 0.f, 1.f + w.damage * 0.08f + w.collateralRadius * 0.2f});
    // damage value: weapon Damages x10 (FUN_100ee440) x the shooter's fire-control multiplier (table 0x10163474)
    static const float kFireControl[11] = {0.24f, 0.2f, 0.3f, 0.5f, 0.7f, 0.8f, 1.f, 1.5f, 2.f, 2.5f, 3.f};
    int fc = 0;
    auto sh = units_.find(shooter);
    if (sh != units_.end()) fc = std::max(-6, std::min(4, sh->second.fireControl));
    float d = w.damage * 10.f * kFireControl[fc + 6] * scale;
    // blast weapons also hurt whatever is near the impact (Collateral_Radius), falling off with distance
    if (w.collateralRadius > 1.f) {
        const auto& objs = scene_->mission().objects;
        for (size_t i = 0; i < objs.size(); i++) {
            if (int(i) == object || (!units_.count(int(i)) && !structure_.count(int(i)))) continue;
            Vec3 q;
            if (!scene_->objectPosition(int(i), q) || scene_->objectHidden(int(i))) continue;
            float dist = length(q + Vec3(0, 2, 0) - p);
            if (dist < w.collateralRadius) applyDamage(int(i), shooter, d * 0.5f * (1.f - dist / w.collateralRadius), p);
        }
    }
    if (object < 0 || size_t(object) >= damage.size()) return;
    if (audio) audio->play("hitbull6.wav", 0.5f, &p, 200.f);
    if (w.type == 0x84 && field(object, 0, 17, 1)) d *= 0.25f;  // Haywire Resistant shrugs off the haywire effect (FUN_100594b0)
    applyDamage(object, shooter, d, p);
}

void Combat::applyDamage(int object, int shooter, float d, const Vec3& p) {
    if (object < 0 || size_t(object) >= damage.size() || d <= 0) return;
    damage[size_t(object)] += d;
    const MissionObject& o = scene_->mission().objects[size_t(object)];
    auto u = units_.find(object);
    if (u == units_.end()) {
        auto st = structure_.find(object);
        if (st != structure_.end()) {  // buildings: structure points
            hits.push_back({object, shooter, 2});
            if (st->second > 0 && (st->second -= d) <= 0) destroyObject(object, p);
        } else if (isUnitClass(o.type) && damage[size_t(object)] >= 600.f) {
            hits.push_back({object, shooter, 2});
            destroyObject(object, p);
        }
        return;
    }
    Unit& un = u->second;
    if (un.hp <= 0) return;
    if (shooter >= 0 && shooter != object) un.lastAttacker = shooter;
    if (un.remote) {  // its owner applies the damage; just show the hit
        hits.push_back({object, shooter, 2});
        return;
    }
    int level = 2;
    if (shooter < 0 && object >= 0) {  // terrain heat: Heat Resistant Armor levels and hostile-environment protection
        d /= 1.f + float(un.perks >> 18 & 15);
        if (un.perks >> 22 & 3) d *= 0.5f;
    }
    {
        int reinforced = 0, weak = 0;  // Reinforced Armor per facing (2 bits x 4), Weak Facing (1 bit x 4)
        for (int k = 0; k < 4; k++) reinforced += int(un.flaws >> (2 * k) & 3), weak += int(un.flaws >> (28 + k) & 1);
        d *= std::max(0.5f, 1.f - 0.04f * float(reinforced)) * (1.f + 0.1f * float(weak));
    }
    if (!un.invincible) {
        float before = un.hp, A = un.hp, lost = 0;
        bool kill = false;
        int critical = -1;  // 0 heavy roll, 1 medium roll
        if (!un.armoured) {
            lost = d;
        } else if (d >= 6.f * A) {
            kill = true;
        } else if (d >= 3.f * A) {
            lost = 20.f, level = 4, critical = 0;
        } else if (d >= 2.f * A) {
            lost = 10.f, level = 3, critical = 1;
        } else if (d >= 0.2f * A) {
            lost = 5.f;
        }
        if (critical >= 0) {
            // critical hit roll: mobility -1/-2, manoeuvre -1/-2, a weapon knocked out, or the gear destroyed
            int r;
            // exposed systems (flaws) make criticals worse (FUN_100618a0 adds them to the roll)
            int exposed = int(un.flaws >> 20 & 1) + int(un.flaws >> 22 & 1) + int(un.flaws >> 23 & 1);
            do r = int(std::lround(float(rand() % 6000) * 0.001f)) + (critical == 0 ? 0 : -1) + exposed;
            while (r < 0);
            // perks that take a critical instead (and are used up): Reinforced Chassis / Rugged Movement System for
            // the drive, Backup Fire Control for the gyro and targeting, Reinforced Crew Compartment for the crew
            auto absorb = [&](uint32_t& w, int bit) {
                if (!(w >> bit & 1)) return false;
                w &= ~(1u << bit);
                if (object == playerObject_) log.push_back("BACKUP SYSTEM ENGAGED");
                return true;
            };
            if (r <= 1 && (absorb(un.flaws, 8) || absorb(un.perks, 30))) r = -1;
            else if ((r == 2 || r == 3) && absorb(un.perks, 3)) r = -1;
            else if (r == 4 && absorb(un.perks, 31)) r = -1;  // Shielded Weapons
            else if (r >= 5 && (absorb(un.flaws, 9) || absorb(un.perks, 16) || absorb(un.perks, 4))) r = -1;  // crew: compartment, medical, backup life support
            // heavy criticals also knock out a system a level (FUN_10062180): ECM, ECCM, jump jets, stealth
            if (critical == 0) switch (rand() % 6) {
                case 1: dropLevel(un.perks, 10, 3); break;
                case 2: dropLevel(un.perks, 13, 3); break;
                case 4: dropLevel(un.perks, 24, 2); break;
                case 5: dropLevel(un.flaws, 10, 3); break;
                default: break;
            }
            switch (r) {
                case -1: break;
                case 0: un.mobility = std::max(0, un.mobility - 1); break;
                case 1: un.mobility = std::max(0, un.mobility - 2); break;
                case 2: un.manoeuvre = std::max(-4, un.manoeuvre - 1); break;
                case 3: un.manoeuvre = std::max(-4, un.manoeuvre - 2); break;
                case 4:
                    if (!un.broken.empty()) {
                        size_t k = size_t(rand()) % un.broken.size();
                        un.broken[k] = true;
                        if (object == playerObject_) log.push_back("WEAPON DAMAGED");
                    }
                    break;
                default: kill = true; break;
            }
            if (object == playerObject_ && r <= 1) log.push_back("DRIVE SYSTEM DAMAGED");
            if (object == playerObject_ && (r == 2 || r == 3)) log.push_back("GYRO DAMAGED");
        }
        un.hp = kill ? 0.f : std::max(0.f, un.hp - lost);
        // locate the hit on the damage hierarchy: the nearest surviving part takes the same wear
        if (lost > 0 && !un.parts.empty()) {
            std::vector<std::string> names;
            for (auto& pt : un.parts)
                if (!pt.gone) names.push_back(pt.frame);
            std::string hitPart = names.empty() ? std::string() : scene_->nearestFrame(object, p, names);
            for (auto& pt : un.parts)
                if (!pt.gone && pt.frame == hitPart) {
                    pt.hp -= pt.region == 'T' ? lost : lost * 2.f;  // limbs wear faster than the hull
                    if (!un.damageTexture.empty())
                        scene_->setFrameDamage(object, pt.frame, un.damageTexture, std::max(0.f, 1.f - pt.hp / pt.maxHp) * 1.3f - 0.2f);
                    if (pt.hp <= 0) partDestroyed(object, un, pt, p);
                    break;
                }
        }
        if (un.hp > 0 && before > un.maxHp * 0.5f && un.hp <= un.maxHp * 0.5f) damaged.push_back(object);
    }
    hits.push_back({object, shooter, level});
    hitsBy[shooter]++;
    if (un.hp <= 0) {
        if (shooter >= 0 && shooter != object) killsBy[shooter]++;
        if (object == playerObject_) {
            destroyed.push_back(object);
            if (!particles || !particles->spawn("EXP_HG", p, 1.5f)) flashes_.push_back({p, 0.f, 6.f});
        } else {
            destroyObject(object, p);
        }
    }
}

void Combat::destroyObject(int object, const Vec3& p) {
    if (scene_->objectHidden(object)) return;
    const MissionObject& o = scene_->mission().objects[size_t(object)];
    if (audio) audio->play("explrg01.wav", 1.f, &p, 800.f);
    // gears fall and stay as wrecks (their bodies play a fall animation); everything else is removed
    if (o.type != 0x48) scene_->hideObject(object);
    destroyed.push_back(object);
    const char* boom = o.type == 0x48 ? "EXP_HG" : o.type == 0x4b ? "EXPTURR" : isUnitClass(o.type) ? "MEDIUM EXPLOSION" : "LARGE EXPLOSION";
    if (!particles || !particles->spawn(boom, p, 1.5f)) flashes_.push_back({p, 0.f, 6.f});
}

void Combat::update(float dt) {
    clock_ += dt;
    for (auto& [obj, u] : units_) {
        for (auto& l : u.loadout) l.cooldown = std::max(0.f, l.cooldown - dt);
        // damaging terrain (lava): units standing on a surface with a damage value take it about once a second
        // while their feet touch the ground (the gear's contact handler, vtable +0x8c from FUN_1003db20)
        if (u.hp > 0 && (u.surfaceTimer -= dt) <= 0) {
            u.surfaceTimer = 1.f;
            Vec3 p;
            if (scene_->objectPosition(obj, p)) {
                const Surface* s = scene_->surfaceAt(p.x, p.z);
                if (s && s->damage > 0 && std::fabs(p.y - scene_->groundHeight(p.x, p.z)) < 1.f) {
                    if (getenv("HG2_DEBUG")) SDL_Log("surface %d damage %d to object %d", s->index, s->damage, obj);
                    applyDamage(obj, -1, float(s->damage), p);
                }
            }
        }
        // damaged gears smoke (DAMAGEL light / DAMAGEH heavy)
        if (particles && u.hp > 0 && u.maxHp > 0 && u.hp < u.maxHp * 0.5f && (u.smokeTimer -= dt) <= 0) {
            u.smokeTimer = 0.6f;
            Vec3 at;
            if (!scene_->frameWorld(obj, "bdb", at) && !scene_->frameWorld(obj, "bda", at)) {
                if (!scene_->objectPosition(obj, at)) continue;
                at.y += 3.f;
            }
            particles->spawn(u.hp < u.maxHp * 0.25f ? "DAMAGEH" : "DAMAGEL", at, 0.7f);
        }
    }
    for (size_t i = 0; i < shots_.size();) {
        Shot& s = shots_[i];
        s.life -= dt;
        bool dead = s.life <= 0;
        if (!s.beam && !dead && s.target >= 0 && !s.jammed && alive(s.target)) {  // guided: turn towards the target
            Vec3 tp;
            if (scene_->objectPosition(s.target, tp)) {
                tp.y += 3.f;
                float sp = length(s.vel);
                Vec3 want = normalize(tp - s.pos), cur = s.vel * (1.f / std::max(1.f, sp));
                Vec3 nd = normalize(cur + (want - cur) * std::min(1.f, dt * 2.5f));
                s.vel = nd * sp;
            }
        }
        if (!s.beam && !dead) {
            Vec3 next = s.pos + s.vel * dt;
            float tg = scene_->terrainRay(s.pos, next), to;
            int obj = scene_->raycast(s.pos, next, s.shooter, to);
            float t = std::min(tg, to);
            if (t <= 1.f) {
                impact(s.pos + (next - s.pos) * t, *s.def, to <= tg ? obj : -1, s.shooter);
                dead = true;
            }
            s.pos = next;
        }
        if (dead) shots_.erase(shots_.begin() + long(i));
        else i++;
    }
    for (size_t i = 0; i < flashes_.size();) {
        flashes_[i].age += dt;
        if (flashes_[i].age > 0.35f) flashes_.erase(flashes_.begin() + long(i));
        else i++;
    }
}

void Combat::draw() {
    for (auto& s : shots_) {
        if (s.beam) scene_->drawExtra(beam_, along(s.pos, s.vel, length(s.vel), 0.25f), false, 0.8f);
        else scene_->drawExtra(tracer_, along(s.pos - normalize(s.vel) * 3.f, s.vel, 3.f, 0.18f), false);
    }
    for (auto& f : flashes_) {
        float sz = f.size * (0.4f + f.age * 3.f);
        scene_->drawExtra(flash_, along(f.pos - Vec3(0, sz * 0.5f, 0), Vec3(0, 1, 0), sz, sz), false,
                          std::max(0.f, 1.f - f.age / 0.35f));
    }
}

std::string Combat::status() const {
    auto it = units_.find(playerObject_);
    if (it == units_.end() || it->second.loadout.empty()) return "no weapons";
    const Unit& u = it->second;
    const Loadout& l = u.loadout[size_t(u.selected)];
    char buf[128];
    int armour = int(100.f * health(playerObject_) + 0.5f);
    if (l.ammo < 0) snprintf(buf, sizeof buf, "armour %d%%  %s  [%d/%zu]", armour, l.def->name.c_str(), u.selected + 1, u.loadout.size());
    else snprintf(buf, sizeof buf, "armour %d%%  %s  ammo %d  [%d/%zu]", armour, l.def->name.c_str(), l.ammo, u.selected + 1, u.loadout.size());
    return buf;
}


void Combat::destroy(int object) {
    if (object < 0 || size_t(object) >= damage.size() || scene_->objectHidden(object)) return;
    auto u = units_.find(object);
    if (u != units_.end()) {
        if (u->second.hp <= 0) return;
        u->second.hp = 0;
    }
    damage[size_t(object)] = 1e6f;
    Vec3 p;
    if (!scene_->objectPosition(object, p)) return kill(object);
    if (object == playerObject_) {
        destroyed.push_back(object);
        if (!particles || !particles->spawn("EXP_HG", p, 1.5f)) flashes_.push_back({p, 0.f, 6.f});
    } else {
        destroyObject(object, p);
    }
}

void Combat::artilleryShell(const Vec3& p) {
    // the shell's weapon comes from a run-time table; a heavy field mortar's blast stands in (Damages 25, radius 20)
    static WeaponDef shell = [] {
        WeaponDef w;
        w.name = "Artillery", w.damage = 25.f, w.collateralRadius = 20.f, w.mortarAngle = -25.f;
        return w;
    }();
    if (audio) audio->play("explrg01.wav", 1.f, &p, 800.f);
    impact(p, shell, -1, -1, 1.f);
}
}  // namespace hg2

namespace hg2 {
void Combat::kill(int object) {
    if (object < 0 || size_t(object) >= damage.size() || scene_->objectHidden(object)) return;
    auto u = units_.find(object);
    if (u != units_.end()) {
        if (u->second.hp <= 0) return;
        u->second.hp = 0;
    }
    damage[size_t(object)] = 1e6f;
    destroyed.push_back(object);
    if (object == playerObject_) return;
    scene_->hideObject(object);
}
}  // namespace hg2
