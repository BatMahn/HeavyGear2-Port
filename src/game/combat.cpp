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
// Weapon effect tables, filled once by FUN_100e4ca0. An ammo class byte c (a projectile's +0x1d8, from the weapon's
// Trail_Effect; the artillery shell sets 0x1f) is read as row c / 10 and size c % 10 (S, M, L); anything past
// row 7 or size 2 falls back to row 0. Each row (0x1026935c, 8 bytes) holds the trail colour, the trail row and the
// explosion row.
struct FxRow { uint32_t color; int trail, explosion; };
constexpr FxRow kFxRows[8] = {
    {0x000000, 0, 0},  // 0x: no trail; MEMI (bullet launchers without a trail code, LRP128)
    {0xffffff, 1, 1},  // 1x: rocket pods (LRP / MRP / HRP): MTMI, MEMI
    {0x969696, 3, 1},  // 2x: bazookas, RFB: MTBA grey, MEMI
    {0x000000, 4, 2},  // 3x: mortars (LGM / HGM / LFM / APM), artillery shells: MTMO, MEMO
    {0x323232, 4, 3},  // 4x: grenade launchers (LGL / HGL / APGL): MTMO, MEGR
    {0xffffff, 5, 1},  // 5x: panzerfausts (LPZ / MPZ / HPZ): MTPA, MEMI
    {0x323232, 1, 4},  // 6x: incendiary rocket pods (IRP / HIRP): MTMI dark, MEIN
    {0x000000, 2, 1},  // 7x: guided missiles (AGM / ABM / ATM / AAM / HATM): MTMIM / MTMIL black, MEMI
};
// trails (0x10269470), 6 rows x {S, M, L}
const char* const kTrails[6][3] = {{nullptr, nullptr, nullptr}, {"MTMIS", "MTMIM", "MTMIL"}, {"MTMIM", "MTMIL", "MTMIL"},
                                   {"MTBAS", "MTBAM", "MTBAL"}, {"MTMOS", "MTMOM", "MTMOL"}, {"MTPAS", "MTPAM", "MTPAL"}};
// explosions (0x10269330), 5 rows x {ground, object}
const char* const kExplosions[5][2] = {{"MEMIG", "MEMIO"}, {"MEMIG", "MEMIO"}, {"MEMOG", "MEMOO"}, {"MEGRG", "MEGRO"}, {"MEING", "MEINO"}};
// muzzle flashes by Flash_Effect (0x10163488); FLARE_LAR has no file in the shipped particle folders
const char* const kFlashes[10] = {"MUZZLE_FLASH", "MUZZLE_FLASH", "MUZZLE_FLASH", "MUZZLE_FLASH", "FLARE_SMA",
                                  "FLARE_LAR",    "MUZZLE_FLASH", "MUZZLE_FLASH", "MUZZLE_FLASH", "MUZZLE_FLASH"};
const FxRow& fxRow(int c, int& size) {
    if (c < 0 || c / 10 >= 8 || c % 10 >= 3) return size = 0, kFxRows[0];
    size = c % 10;
    return kFxRows[c / 10];
}
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
    it->second.maneuverNow = maneuver;
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
    u.maneuverNow = def.maneuverability;
    u.movement = u.movementBase = std::max(1, def.movement1);
    {
        const auto& objs = scene_->mission().objects;
        u.gear = object >= 0 && size_t(object) < objs.size() && objs[size_t(object)].type == 0x48;
    }
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
    // criticals take the Movement stat down (FUN_10062430 0x80 / 0x100 / 0x40000 / 0x8000), and vmax is Movement x 6
    // km/h, so the body's speed scales with what is left of it
    auto it = units_.find(object);
    if (it == units_.end() || it->second.movementBase <= 0) return 1.f;
    return float(it->second.movement) / float(it->second.movementBase);
}

int Combat::maneuverNow(int object) const {
    auto it = units_.find(object);
    return it == units_.end() ? 0x7fff : it->second.maneuverNow;
}

bool Combat::weaponBroken(int object, int index) const {
    auto it = units_.find(object);
    return it != units_.end() && index >= 0 && size_t(index) < it->second.broken.size() && it->second.broken[size_t(index)];
}

bool Combat::sensorsOkRaw(int object) const {
    auto it = units_.find(object);
    if (it == units_.end()) return true;
    if (it->second.sensorsLost) return false;
    for (auto& p : it->second.parts)
        if (p.system == 3 && p.gone) return false;
    return true;
}

bool Combat::sensorsOk(int object) const {
    auto it = units_.find(object);
    if (it == units_.end()) return true;
    if (it->second.sensorsLost) return false;    // FUN_100622b0 took the sensors (and the backup)
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
    if (audio) audio->play("expmed01.wav", 0.8f, &at, 500.f);  // (explsm01.wav, used before, does not exist)
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

int Combat::autoBurst(int shooter) const {
    auto it = units_.find(shooter);
    if (it == units_.end() || it->second.loadout.empty()) return 1;
    return std::max(1, it->second.loadout[size_t(it->second.selected)].def->autoBurst);
}

float Combat::firepower(int object) const {
    auto it = units_.find(object);
    if (it == units_.end()) return 0;
    float sum = 0;
    for (auto& l : it->second.loadout)
        if (l.def && l.ammo != 0) sum += l.def->damage;
    return sum;
}

float Combat::hitpoints(int object) const {
    auto it = units_.find(object);
    return it == units_.end() ? 0.f : std::max(0.f, it->second.hp);
}

float Combat::muzzleVelocity(int shooter) const {
    auto it = units_.find(shooter);
    if (it == units_.end() || it->second.loadout.empty()) return 0;
    const Loadout& l = it->second.loadout[size_t(it->second.selected)];
    return l.def && l.def->type != 2 ? l.def->velocity : 0.f;
}

int Combat::ammoLeft(int shooter) const {
    auto it = units_.find(shooter);
    if (it == units_.end() || it->second.loadout.empty()) return 0;
    return it->second.loadout[size_t(it->second.selected)].ammo;
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
bool Combat::testWeapon(const std::string& name) {
    auto& u = units_[playerObject_];
    if (u.loadout.empty()) return false;
    for (auto& w : weapons_)
        if (w.name == name) {
            Loadout& l = u.loadout[size_t(u.selected)];
            l.def = &w, l.ammo = -1, l.cooldown = 0;
            return true;
        }
    return false;
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
    // the weapon's cadence (FUN_100ece00 / FUN_100ef270 / FUN_100ecd40): Next_Round_Time between rounds; after Burst
    // rounds a NextBurst pause, after which Next_Round_Time runs again
    l.cooldown = std::max(0.03f, w.nextRoundTime);
    if (w.burst > 0 && ++l.burstCount >= w.burst) l.burstCount = 0, l.cooldown += std::max(0.f, w.nextBurst);
    it->second.firedAt = clock_;
    if (l.ammo > 0) l.ammo--;
    shotsBy[shooter]++;
    Vec3 dir = normalize(aim - muzzle);
    if (audio) audio->play(w.fireSound, shooter == playerObject_ ? 0.6f : 0.8f, &muzzle, 500.f);
    // muzzle flash by Flash_Effect (FUN_100ef610, only when it is set: launchers have none)
    if (particles && w.type != 2 && w.flashEffect >= 0 && w.flashEffect < 10 &&
        !particles->spawn(kFlashes[w.flashEffect], muzzle, 0.3f, dir, 0.3f) && w.flashEffect == 5)
        particles->spawn("FLARE_SMA", muzzle, 0.45f, dir, 0.3f);
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
    if (explosive(w)) sh.trail = startTrail(std::max(0, w.trailEffect), muzzle);
    shots_.push_back(sh);
    return true;
}

bool Combat::explosive(const WeaponDef& w) {
    // launcher projectiles (vtable 0x101498f0 family: rocket pods, bazookas, mortars, grenade launchers, missiles)
    // carry an ammo class and explode; cannon, machine-gun, laser and rail rounds are bullets (0x101487a8) that
    // spark. Launchers whose weapon has no Trail_Effect get class 0 (FUN_100e8570); thrown grenades explode too.
    return !w.beam() && (w.trailEffect >= 0 || w.collateralRadius > 0.f || w.type == 3 || w.type == 7 || w.type == 0x84);
}

int Combat::startTrail(int c, const Vec3& pos) {
    // FUN_100ef740: in space every trail is MT_SPACE, under water MT_WATER, else the table's trail and colour
    if (!particles) return 0;
    int size;
    const FxRow& row = fxRow(c, size);
    if (space_) return particles->attach("MT_SPACE", pos);
    if (scene_->mission().hasWater && pos.y < scene_->waterLevel()) return particles->attach("MT_WATER", pos);
    const char* name = kTrails[row.trail][size];
    return name ? particles->attach(name, pos, 1.f, row.color) : 0;
}

void Combat::explosionEffect(int c, const Vec3& p, bool onObject) {
    // FUN_100e53e0 (object, vtable +0xe4), FUN_100e5530 (ground, +0xe8), FUN_100e5880 (end of flight, +0xec: the
    // object one): explosion FUN_100f3140 at scale 1 with the projectile's blast radius, and expdyna2.wav
    int size;
    const FxRow& row = fxRow(c, size);
    const char* name = kExplosions[row.explosion][onObject ? 1 : 0];
    if (!particles || !particles->spawn(name, p, 1.f)) flashes_.push_back({p, 0.f, 6.f});
    if (audio) audio->play("expdyna2.wav", 1.f, &p, 800.f);
}

void Combat::bulletEffect(const Vec3& p, bool onObject) {
    // bullets (FUN_100df390 object, FUN_100df4e0 ground): SPARKO on objects; on the ground the surface's DustIndex
    // picks SPARK0 / SPARK1 tinted with its DustColor (any other index: SPARKO), and under water SPARKU
    if (!particles) return;
    if (onObject) {
        particles->spawn("SPARKO", p, 1.f);
        return;
    }
    const Surface* s = scene_->surfaceAt(p.x, p.z);
    if (scene_->mission().hasWater && p.y < scene_->waterLevel()) {
        particles->spawn("SPARKU", p, 1.f, Vec3(0, 1, 0), 1.f, s ? s->dustColor : 0u);
        return;
    }
    if (s && (s->dustIndex == 0 || s->dustIndex == 1)) particles->spawn(s->dustIndex ? "SPARK1" : "SPARK0", p, 1.f, Vec3(0, 1, 0), 1.f, s->dustColor);
    else particles->spawn("SPARKO", p, 1.f);
}

void Combat::impact(const Vec3& p, const WeaponDef& w, int object, int shooter, float scale, bool expired) {
    if (getenv("HG2_DEBUG")) fprintf(stderr, "impact %s obj %d by %d at %.0f %.0f %.0f\n", w.name.c_str(), object, shooter, p.x, p.y, p.z);
    if (explosive(w)) explosionEffect(std::max(0, w.trailEffect), p, object >= 0 || expired);
    else if (!expired) bulletEffect(p, object >= 0);
    // damage value: weapon Damages x10 (FUN_100ee440) x the shooter's fire-control multiplier (table 0x10163474)
    static const float kFireControl[11] = {0.24f, 0.2f, 0.3f, 0.5f, 0.7f, 0.8f, 1.f, 1.5f, 2.f, 2.5f, 3.f};
    int fc = 0;
    auto sh = units_.find(shooter);
    if (sh != units_.end()) fc = std::max(-6, std::min(4, sh->second.fireControl));
    float d = w.damage * 10.f * kFireControl[fc + 6] * scale;
    // haywire rounds (FUN_100f3da0) carry the 0x40 flag and the critical mask 0x35; everything else 0xe
    const bool haywire = w.type == 0x84;
    const uint32_t mask = haywire ? 0x35u : 0xeu;
    // the explosion (FUN_100f3140 with the weapon's Collateral_Radius, damage by FUN_100f3430): everything else
    // within the radius takes the same damage, delayed by its distance / 60
    if (w.collateralRadius > 1.f) {
        const auto& objs = scene_->mission().objects;
        for (size_t i = 0; i < objs.size(); i++) {
            if (int(i) == object || (!units_.count(int(i)) && !structure_.count(int(i)))) continue;
            Vec3 q;
            if (!scene_->objectPosition(int(i), q) || scene_->objectHidden(int(i))) continue;
            float dist = length(q - p);
            if (dist <= w.collateralRadius) blasts_.push_back({int(i), dist / 60.f, d, p, shooter, mask, haywire});
        }
    }
    if (object < 0 || size_t(object) >= damage.size()) return;
    if (audio && !explosive(w)) audio->play("hitbull6.wav", 0.5f, &p, 200.f);
    applyDamage(object, shooter, d, p, mask, haywire);
}

void Combat::applyDamage(int object, int shooter, float d, const Vec3& p, uint32_t mask, bool haywire) {
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
    if (!un.invincible) {
        // FUN_100f3630: the hit's direction against the unit's axes; within 60 deg of its fore-aft axis it is a
        // front / back hit (word bits 1 / 2), otherwise a side hit (8 / 4)
        int dirBit = 0;
        {
            Vec3 tp, src = p;
            if (shooter >= 0 && shooter != object) scene_->objectPosition(shooter, src);
            Vec3 back, side;
            if (scene_->objectPosition(object, tp) && scene_->objectAxis(object, 2, back) && scene_->objectAxis(object, 0, side)) {
                Vec3 dv = tp - src;
                if (length(dv) > 1e-3f) {
                    dv = normalize(dv);
                    float fb = dot(dv, back);
                    dirBit = std::fabs(fb) >= 0.5f ? (fb < 0 ? 1 : 2) : (dot(dv, side) < 0 ? 8 : 4);
                }
            }
        }
        float before = un.hp, A = un.hp, lost = 0;
        // the facing's Reinforced Armor level adds 2 per level to the armour the hit is measured against (+0xc34)
        if (dirBit) {
            int k = dirBit == 1 ? 0 : dirBit == 2 ? 1 : dirBit == 4 ? 2 : 3;
            A += 2.f * float(un.flaws >> (2 * k) & 3);
        }
        bool kill = false;
        uint32_t crit = 0;
        if (!un.armoured) {
            lost = d;
        } else {
            // FUN_100618a0 / FUN_10058fd0: D >= 6A destroys, >= 3A heavy, >= 2A medium, >= 0.2A light
            const int sev = d >= 6.f * A ? 6 : d >= 3.f * A ? 3 : d >= 2.f * A ? 2 : d >= 0.2f * A ? 1 : 0;
            crit = criticalRoll(object, un, float(sev), mask);
            // heavy hits on a gear from the front or back knock it over (single player, ECY 0x11 / 0x12)
            if ((crit & 0x40000000u) && un.gear && !multiplayer && !un.knock && (dirBit == 1 || dirBit == 2))
                un.knock = dirBit == 1 ? 0x11 : 0x12;
            level = sev >= 3 ? 4 : sev == 2 ? 3 : 2;
            float hp0 = un.hp;
            applyCritical(object, un, crit, kill);
            // haywire (damage word bit 0x40, FUN_10063460): unless Haywire Resistant (perk bit 17), another critical
            // roll, and a third with flaw bit 27
            if (haywire && un.gear && !(un.perks >> 17 & 1) && !kill) {
                applyCritical(object, un, criticalRoll(object, un, float(sev), mask), kill);
                if ((un.flaws >> 27 & 1) && !kill) applyCritical(object, un, criticalRoll(object, un, float(sev), mask), kill);
            }
            lost = hp0 - un.hp;
            un.hp = hp0;
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

uint32_t Combat::criticalRoll(int object, Unit& u, float level, uint32_t mask) {
    // level: 6 destroyed (D >= 6A), 3 heavy (>= 3A), 2 medium (>= 2A), 1 light (>= 0.2A), 0 nothing
    auto roll = [](int range) { return int(std::lround(float(rand() % range) * 0.001f)); };  // round(0 .. range/1000)
    const int lv = int(level);
    if (lv >= 6) return 0x100000;
    if (lv <= 0) return 0;
    if (lv == 1) return 0x10000000;
    const bool heavy = lv >= 3;
    const uint32_t base = heavy ? 0x40000000u : 0x20000000u;
    if (!u.gear) {
        // FUN_10058fd0 (vehicles, turrets): one roll on a single table, a step lower for medium hits
        for (;;) switch (roll(6000) + (heavy ? 0 : -1)) {
            case 0: return base | 0x10000;
            case 1: return base | 0x20000;
            case 2: return base | 0x800;
            case 3: return base | 0x1000;
            case 4: return base | 0x2000;
            case 5: return 0x80000;
            case 6: return 0x100000;
            default: break;
        }
    }
    // FUN_100618a0: a random system group (bit 0 .. 3 medium, 0 .. 4 heavy, rounded), affected only if the
    // weapon's mask has it; then a roll on that group's table, shifted by the matching Exposed flaw
    uint32_t P = u.perks, F = u.flaws;
    uint32_t g = 1u << roll(heavy ? 5000 : 4000);
    if (!(mask & g)) return base;
    const int shift = heavy ? 0 : -1;
    switch (g) {
        case 1:  // fire control / weapons (Exposed Fire Control, flaw bit 20)
            for (;;) switch (roll(6000) + int(F >> 20 & 1) + shift) {
                case 0: case 1: return base | 0x800;
                case 2: return base | 0x1000;
                case 3: case 4: return base | 0x2000;
                case 5:
                    if (P >> 3 & 1) { u.perks &= ~(1u << 3); return base; }  // Backup Fire Control
                    return base | 0x4000;
                case 6: return base | 0x8000;
                default: break;
            }
        case 2:  // movement (Reinforced Chassis flaw-word bit 8 takes it once; Exposed Movement, bit 23)
            if (F >> 8 & 1) { u.flaws &= ~(1u << 8); return base; }
            for (;;) switch (roll(6000) + int(F >> 23 & 1) + shift) {
                case 0: case 1: return base | 0x80;
                case 2: return base | 0x10000;
                case 3: return base | 0x20000;
                case 4: return base | 0x100;
                case 5: return base | 0x80000;
                case 6: return base | 0x100000;
                default: break;
            }
        case 4:  // crew (flaw bit 21: knocked down; Reinforced Crew Compartment bit 9, Emergency Medical perk 16)
            if (!heavy) {
                if (F >> 21 & 1) return 0x80000;
                if (F >> 9 & 1) { u.flaws &= ~(1u << 9); return base; }
                if (P >> 16 & 1) { u.perks &= ~(1u << 16); return base; }
                return base | 0x40;
            }
            if (F >> 9 & 1) { u.flaws &= ~(1u << 9); return base; }
            return 0x80000;
        case 8:  // power plant (Rugged Movement System perk 30 takes it once; Exposed Power Plant, flaw bit 22)
            if (P >> 30 & 1) { u.perks &= ~(1u << 30); return base; }
            if (!heavy) return base | (F >> 22 & 1 ? 0x100u : 0x80u);
            return base | (F >> 22 & 1 ? 0x40000u : 0x100u);  // (flaw 22: 0x3ff00 + 0x100 = movement to 1)
        case 0x10: return base | (heavy ? 0x200 : 0x400);  // auxiliary systems
        default: return base;
    }
}

void Combat::applyCritical(int object, Unit& u, uint32_t bits, bool& kill) {
    // FUN_10062430 (gears) / FUN_10059240 (other units), lowest bit first
    auto msg = [&](const char* m) { if (object == playerObject_) log.push_back(m); };
    auto fc = [&](int step) { u.fireControl = std::max(-5, u.fireControl - step); };
    auto immobile = [&]() { u.movement = 1; };
    for (uint32_t b = 1; b && bits; b <<= 1) {
        if (!(bits & b)) continue;
        bits &= ~b;
        switch (b) {
            case 0x80: u.movement = std::max(1, u.movement - 1), msg("DRIVE SYSTEM DAMAGED"); break;
            case 0x100: u.movement = std::max(1, u.movement >> 1), msg("DRIVE SYSTEM DAMAGED"); break;
            case 0x200:  // FUN_100622b0: one system lost entirely
                for (;;) {
                    int r = int(std::lround(float(rand() % 7000) * 0.001f));
                    if (r == 1) { u.perks &= ~(7u << 10); break; }                      // ECM
                    if (r == 2) { u.perks &= ~(7u << 13); break; }                      // ECCM
                    if (r == 3) { if (u.perks >> 4 & 1) u.perks &= ~(1u << 4); else u.flaws &= ~(1u << 14); break; }
                    if (r == 4) { u.perks &= ~(3u << 24); break; }                      // jump jets
                    if (r == 5) { if (u.perks >> 5 & 1) u.perks &= ~(1u << 5); else u.sensorsLost = true; break; }
                    if (r == 6) { u.flaws &= ~(7u << 10); break; }                      // stealth
                    if (r == 7) { if (u.perks >> 2 & 1) u.perks &= ~(1u << 2); else u.commsLost = true; break; }
                }
                break;
            case 0x400:  // FUN_10062180: one system down a level
                for (bool done = false; !done;) {
                    int r = int(std::lround(float(rand() % 7000) * 0.001f));
                    done = true;
                    if (r == 1) dropLevel(u.perks, 10, 3);
                    else if (r == 2) dropLevel(u.perks, 13, 3);
                    else if (r == 4) dropLevel(u.perks, 24, 2);
                    else if (r == 6) dropLevel(u.flaws, 10, 3);
                    else if (r == 0 || r == 7) done = false;
                }
                break;
            case 0x800: fc(1), msg("FIRE CONTROL DAMAGED"); break;
            case 0x1000: fc(u.fireControl < -3 ? 1 : 2), fc(1), msg("FIRE CONTROL DAMAGED"); break;
            case 0x2000:
                if (u.gear && (u.perks >> 31 & 1)) { u.perks &= ~(1u << 31); break; }  // Shielded Weapons
                if (!u.broken.empty()) {
                    u.broken[size_t(rand()) % u.broken.size()] = true;
                    msg("WEAPON DAMAGED");
                } else if (!u.gear && u.maneuverNow > 0) {
                    u.maneuverNow--;
                }
                break;
            case 0x4000: fc(1), fc(1), msg("FIRE CONTROL DAMAGED"); break;
            case 0x8000:  // cockpit (Backup Life Support perk bit 0 takes it once)
                if (u.perks & 1) { u.perks &= ~1u; break; }
                if ((u.flaws >> 24 & 1) ? (rand() & 2) != 0 : (rand() & 1) == 0) kill = true;
                else immobile(), msg("DRIVE SYSTEM DAMAGED");
                break;
            case 0x10000: if (u.maneuverNow > 0) u.maneuverNow--; msg("GYRO DAMAGED"); break;
            case 0x20000: u.maneuverNow -= u.maneuverNow < 2 ? 1 : 2; msg("GYRO DAMAGED"); break;
            case 0x40000: immobile(); break;
            case 0x80000:
                if (!u.gear) { kill = true; break; }
                if (!multiplayer && !u.knock) u.knock = (rand() & 1) ? 0x13 : 0x14;
                break;
            case 0x100000: kill = true; break;
            // armour: gears -3 / -8 / -25 (FUN_10062430), other units -5 / -10 / -20 (FUN_10059240), on armour x 10
            case 0x10000000: u.hp = std::max(0.f, u.hp - (u.gear ? 3.f : 5.f)); break;
            case 0x20000000: u.hp = std::max(0.f, u.hp - (u.gear ? 8.f : 10.f)); break;
            case 0x40000000: u.hp = std::max(0.f, u.hp - (u.gear ? 25.f : 20.f)); break;
            default: break;
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
        if (particles && !smoking_.count(obj) && u.hp > 0 && u.maxHp > 0 && u.hp < u.maxHp * 0.5f && (u.smokeTimer -= dt) <= 0) {
            u.smokeTimer = 0.6f;
            Vec3 at;
            if (!scene_->frameWorld(obj, "bdb", at) && !scene_->frameWorld(obj, "bda", at)) {
                if (!scene_->objectPosition(obj, at)) continue;
                at.y += 3.f;
            }
            particles->spawn(u.hp < u.maxHp * 0.25f ? "DAMAGEH" : "DAMAGEL", at, 0.7f);
        }
    }
    // script smoke (SetSmoking): attached at the unit's damage frame, or 0.65 x its radius above the origin
    for (auto& [obj, level] : smoking_) {
        if (level == 0 || !particles || scene_->objectHidden(obj) || (smokeTimers_[obj] -= dt) > 0) continue;
        smokeTimers_[obj] = 0.6f;
        Vec3 at;
        if (!scene_->frameWorld(obj, "bdb", at) && !scene_->frameWorld(obj, "bda", at)) {
            if (!scene_->objectPosition(obj, at)) continue;
            at.y += 0.65f * std::max(2.f, scene_->objectRadius(obj));
        }
        particles->spawn(level == 2 ? "DAMAGEH" : "DAMAGEL", at, 1.5f);
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
        if (!s.beam && dead && explosive(*s.def)) impact(s.pos, *s.def, -1, s.shooter, 1.f, true);  // end of flight
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
            if (particles && s.trail) particles->move(s.trail, s.pos, s.vel);
        }
        if (dead) {
            if (particles && s.trail) particles->detach(s.trail);
            shots_.erase(shots_.begin() + long(i));
        }
        else i++;
    }
    updateShells(dt);
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
    for (auto& s : shells_)
        if (length(s.vel) > 1.f) scene_->drawExtra(tracer_, along(s.pos - normalize(s.vel) * 2.f, s.vel, 2.f, 0.3f), false);
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
    // the shell (vtable 0x10140a08) carries no MstrWpns weapon: ammo class 0x1f (mortar, medium) picks the
    // MTMOM trail and the MEMOG / MEMOO explosions from the effect tables FUN_100e4ca0 fills; blast radius 30,
    // damage word 0x190080 >> 14 = 100, doubled by the explosion = 200 (Damages 20)
    shells_.push_back({p, Vec3(0, 0, 0), startTrail(0x1f, p)});
}

void Combat::blast(const Vec3& p, float radius, float damage, int object) {
    explosionEffect(0x1f, p, object >= 0);
    const auto& objs = scene_->mission().objects;
    for (size_t i = 0; i < objs.size(); i++) {
        if (!units_.count(int(i)) && !structure_.count(int(i))) continue;
        Vec3 q;
        if (!scene_->objectPosition(int(i), q) || scene_->objectHidden(int(i))) continue;
        float dist = int(i) == object ? 0.f : length(q - p);
        if (dist <= radius) blasts_.push_back({int(i), dist / 60.f, damage, p});
    }
    if (getenv("HG2_DEBUG")) SDL_Log("blast at %.0f %.0f %.0f (object %d): %zu in range", p.x, p.y, p.z, object, blasts_.size());
}

void Combat::updateShells(float dt) {
    for (size_t i = 0; i < shells_.size();) {
        Shell& s = shells_[i];
        s.vel.y -= 20.f * dt;  // FUN_100e7910: 20 m/s^2
        Vec3 next = s.pos + s.vel * dt;
        if (particles && s.trail) particles->move(s.trail, s.pos, s.vel);
        float tg = scene_->terrainRay(s.pos, next), to;
        int obj = scene_->raycast(s.pos, next, -1, to);
        float t = std::min(tg, to);
        if (t <= 1.f || s.pos.y < -1000.f) {
            blast(s.pos + (next - s.pos) * std::min(t, 1.f), 30.f, 200.f, to <= tg ? obj : -1);
            if (particles && s.trail) particles->detach(s.trail);
            shells_.erase(shells_.begin() + long(i));
            continue;
        }
        s.pos = next;
        i++;
    }
    for (size_t i = 0; i < blasts_.size();) {
        if ((blasts_[i].wait -= dt) > 0) { i++; continue; }
        Blast b = blasts_[i];
        blasts_.erase(blasts_.begin() + long(i));
        if (!scene_->objectHidden(b.object)) applyDamage(b.object, b.shooter, b.damage, b.at, b.mask, b.haywire);
    }
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
