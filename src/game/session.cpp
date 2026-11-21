#include "session.h"

#include <zlib.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <random>

#include "../net/net.h"
#include "../script/system.h"
#include "ai.h"
#include "combat.h"
#include "fountains.h"
#include "hud.h"
#include "particles.h"
#include "player.h"
#include "scene.h"

namespace hg2 {

namespace {
constexpr float kPi = 3.14159265358979f;
constexpr float kDeg = kPi / 180.f;

bool writePNG_(const std::string& path, const std::vector<uint8_t>& rgba, int w, int h) {
    std::vector<uint8_t> raw;
    raw.reserve(size_t(w * 4 + 1) * h);
    for (int y = 0; y < h; y++) {
        raw.push_back(0);
        raw.insert(raw.end(), rgba.begin() + size_t(y) * w * 4, rgba.begin() + size_t(y + 1) * w * 4);
    }
    uLongf zl = compressBound(uLong(raw.size()));
    std::vector<uint8_t> z(zl);
    compress2(z.data(), &zl, raw.data(), uLong(raw.size()), 6);
    z.resize(zl);
    FILE* f = fopen(path.c_str(), "wb");
    if (!f) return false;
    auto be32 = [](uint32_t v, uint8_t* o) { o[0] = uint8_t(v >> 24), o[1] = uint8_t(v >> 16), o[2] = uint8_t(v >> 8), o[3] = uint8_t(v); };
    auto chunk = [&](const char* type, const uint8_t* d, size_t n) {
        uint8_t hdr[8];
        be32(uint32_t(n), hdr);
        memcpy(hdr + 4, type, 4);
        fwrite(hdr, 1, 8, f);
        if (n) fwrite(d, 1, n, f);
        uLong c = crc32(0, hdr + 4, 4);
        if (n) c = crc32(c, d, uInt(n));
        uint8_t cb[4];
        be32(uint32_t(c), cb);
        fwrite(cb, 1, 4, f);
    };
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 13, 10, 26, 10};
    fwrite(sig, 1, 8, f);
    uint8_t ihdr[13];
    be32(uint32_t(w), ihdr);
    be32(uint32_t(h), ihdr + 4);
    ihdr[8] = 8, ihdr[9] = 6, ihdr[10] = ihdr[11] = ihdr[12] = 0;
    chunk("IHDR", ihdr, 13);
    chunk("IDAT", z.data(), z.size());
    chunk("IEND", nullptr, 0);
    fclose(f);
    return true;
}

struct Camera {
    Vec3 pos;
    float yaw = 0, pitch = -0.2f;  // radians; yaw 0 looks along +Z
    Vec3 forward() const { return {std::sin(yaw) * std::cos(pitch), std::sin(pitch), std::cos(yaw) * std::cos(pitch)}; }
};

}  // namespace

bool writePngFile(const std::string& path, const std::vector<uint8_t>& rgba, int w, int h) { return writePNG_(path, rgba, w, h); }

bool masterGear(const FileSystem& fs, int index, Bytes& out) {
    static Bytes cache;
    static std::vector<std::pair<size_t, size_t>> entries;
    if (entries.empty()) {
        if (!fs.read("MstrGear.bin", cache) || cache.size() < 16) return false;
        Reader r(cache);
        r.u32(), r.u32(), r.u32();
        uint32_t n = r.u32();
        for (uint32_t i = 0; i < n && r.ok(4); i++) {
            uint32_t sz = r.u32();
            if (!r.ok(sz)) break;
            entries.push_back({size_t(r.p - cache.data()), sz});
            r.skip(sz);
        }
    }
    if (index < 0 || size_t(index) >= entries.size()) return false;
    out.assign(cache.begin() + long(entries[size_t(index)].first), cache.begin() + long(entries[size_t(index)].first + entries[size_t(index)].second));
    return true;
}

static MissionResult play(AppContext& app, const MissionSetup& setup, const TestOptions& T);

MissionResult runMission(AppContext& app, const MissionSetup& setup, const TestOptions* test) {
    TestOptions none;
    Renderer::ResourceMark mark = app.r.mark();
    app.audio.effectsVolume = app.settings.effects;
    MissionResult res = play(app, setup, test ? *test : none);
    if (app.audioOk) app.audio.stopAll();
    app.r.setOverlay({});
    app.r.setDynamic({}, {});
    app.r.releaseTo(mark);
    return res;
}

static MissionResult play(AppContext& app, const MissionSetup& setup, const TestOptions& T) {
    MissionResult res;
    FileSystem& fs = app.fs;
    Renderer& r = app.r;
    const bool offscreen = app.offscreen;
    const int width = app.width, height = app.height;
    Camera cam;
    bool camSet = T.camSet;
    if (camSet) cam.pos = {T.cam[0], T.cam[1], T.cam[2]}, cam.yaw = T.cam[3] * kDeg, cam.pitch = T.cam[4] * kDeg;
    bool external = T.external || app.settings.externalView;
    std::string err;
    Scene scene(fs, r);
    // shell choices go into the mission before anything is built
    std::vector<int> mates;            // squad mate objects
    // instant action (the sim's launch type 4 -> config +0x2a, FUN_100455b0): the enemies, which come back at the
    // arena's spawn points until each has been destroyed `setup.waves` times (config +0x80, the shell's fire teams)
    const bool instant = !setup.enemies.empty() && setup.waves > 0;
    std::vector<int> iaUnits;
    int userTeam = 100;
    auto prepare = [&](Mission& m) {
        int user = -1;
        for (size_t i = 0; i < m.objects.size(); i++)
            if (m.objects[i].userHeader) {
                user = int(i);
                break;
            }
        if (user < 0) return;
        Bytes g;
        if (setup.player.chassis >= 0 && masterGear(fs, setup.player.chassis, g)) m.objects[size_t(user)].defIndex = m.addDef(g.data(), g.size());
        const MissionObject base = m.objects[size_t(user)];
        userTeam = base.team;
        Vec3 ax0, ax2, at(float(base.pos[0]), float(base.pos[1]), float(base.pos[2]));
        AiSystem::yawAxes(float(base.rot[0]) * kDeg, ax0, ax2);
        for (size_t i = 0; i < setup.squad.size(); i++) {
            MissionObject o;
            o.type = 0x48;
            o.team = base.team;
            o.name = "mate" + std::to_string(i + 1);
            for (int k = 0; k < 3; k++) o.rot[k] = base.rot[k];
            o.defIndex = base.defIndex;
            Bytes mg;
            if (setup.squad[i].gear.chassis >= 0 && masterGear(fs, setup.squad[i].gear.chassis, mg)) o.defIndex = m.addDef(mg.data(), mg.size());
            // FUN_100cc8f0 (ForceUserSquadFormation, also run as the mission starts): member c of each group straight
            // onto its formation slot; mates join groups A and B in turn
            Vec3 p = AiSystem::formationPoint(int(i % 2), int(i / 2), at, ax0, ax2);
            o.pos[0] = p.x, o.pos[1] = base.pos[1], o.pos[2] = p.z;
            m.objects.push_back(o);
            mates.push_back(int(m.objects.size()) - 1);
        }
        // instant-action enemies: on the team after the user's; placed at the spawn points once the world is built
        for (size_t i = 0; instant && i < setup.enemies.size(); i++) {
            Bytes g;
            if (!masterGear(fs, setup.enemies[i].gear.chassis, g)) continue;
            MissionObject o;
            o.type = 0x48;
            o.team = base.team + 1;
            o.name = "enemy" + std::to_string(i + 1);
            o.defIndex = m.addDef(g.data(), g.size());
            for (int k = 0; k < 3; k++) o.pos[k] = base.pos[k], o.rot[k] = base.rot[k];
            m.objects.push_back(o);
            iaUnits.push_back(int(m.objects.size()) - 1);
        }
    };
    if (!scene.load(setup.mission, &err, prepare)) {
        res.error = err;
        return res;
    }
    struct World : ScriptWorld {
        const Mission* m = nullptr;
        bool unitPosition(int unit, float out[3]) override {
            Vec3 p;
            if (pl && unit == player && pl->active()) {
                p = pl->position();
                out[0] = p.x, out[1] = p.y, out[2] = p.z;
                return true;
            }
            if (ai && ai->position(unit, p)) {
                out[0] = p.x, out[1] = p.y, out[2] = p.z;
                return true;
            }
            if (unit < 0 || size_t(unit) >= m->objects.size()) return false;
            for (int k = 0; k < 3; k++) out[k] = float(m->objects[size_t(unit)].pos[k]);
            return true;
        }
        const Scene* scene = nullptr;
        AiSystem* ai = nullptr;
        Combat* combat = nullptr;
        int player = -1;
        bool control = true;
        std::vector<std::pair<std::string, int>> squadPilots;  // name, gear object (the campaign roster's squad list)
        bool rosterOff = false;  // network play or instant action (+0x1b / +0x2a): FUN_100842e0 keeps no status
        int pilotStatus(const std::string& name) override {
            // FUN_100842e0: per squad pilot, the gear's alive flag (+8) false or going down (+0xc10) -> -1, ejected
            // (+0xc11) -> 1, else 0; FUN_10084170 then finds the pilot by (lower-case) name
            if (rosterOff || !combat) return 0;
            for (auto& [n, obj] : squadPilots)
                if (lower(n) == lower(name)) return combat->ejected(obj) ? 1 : !combat->alive(obj) ? -1 : 0;
            return 0;
        }
        bool isAlive(int unit) override {
            if (unit < 0 || (scene && scene->objectHidden(unit))) return false;
            return !combat || combat->alive(unit);
        }
        bool userAlive() override {
            bool a = player < 0 || !combat || combat->alive(player);
            if (getenv("HG2_SIGDBG")) SDL_Log("UserAlive -> %d (player %d)", int(a), player);
            return a;
        }
        int playerUnit() override { return player; }
        void unitSleep(int u, bool on) override { if (ai) ai->sleep(u, on); }
        void unitActivate(int u, bool on) override { if (ai) ai->sleep(u, !on); }
        // FollowPath on the user's own gear (FUN_100d5590 / FUN_100d5380 give the order to the unit's brain, +0xd8c,
        // which the user's gear has too): the brain walks the gear along the path while the user isn't driving it
        // (cinematic mode or SetUserControl(0)), e.g. S1J's intro walk along "user1" under the camera fly-by
        std::vector<Vec3> userPath;
        size_t userPathIdx = 0;
        struct Flyer { std::vector<Vec3> pts; size_t idx = 0; float speed = 40.f; Vec3 pos; float yaw = 0; };
        std::map<int, Flyer> flyers;
        // maglev trains (class 0x13e, FUN_10073a00) and elevators (0x60, FUN_10073ef0, same path slots): AssignPath stores the path's points offset by the train's own
        // position (FUN_10073bd0); Forward(n) / Backward(n) (vtable +0x224 / +0x228) run it n points along it;
        // the update (FUN_10073d10) moves at speed x dt toward the next point
        struct Rail { std::vector<Vec3> pts; int cur = 0, target = 0, dir = 0, count = 0; float speed = 20.f; Vec3 pos; float yaw = 0; };
        std::map<int, Rail> rails;
        void unitGoto(int u, const float p[3]) override {
            if (u == player && player >= 0) userPath = {Vec3(p[0], p[1], p[2])}, userPathIdx = 0;
            else if (ai) ai->gotoPoint(u, Vec3(p[0], p[1], p[2]));
        }
        bool unitFollowPath(int u, const std::string& n) override {
            if (u == player && player >= 0) {
                const MissionPath* pa = m->path(n);
                if (pa) {
                    userPath.clear(), userPathIdx = 0;
                    for (auto& q : pa->points) userPath.push_back(Vec3(q[0], q[1], q[2]));
                } else if (verbose) {
                    SDL_Log("[script] FollowPath %s for the user: no such path", n.c_str());
                }
                return pa != nullptr;
            }
            bool ok = ai && ai->followPath(u, n);
            // a unit on team 99 (neutral to the AI's targeting) that a script sends along a path, e.g. H07's escort
            // gear: it becomes a simulated unit now so it can walk
            if (!ok && ai && m->path(n) && u >= 0 && size_t(u) < m->objects.size() && m->objects[size_t(u)].team < 100 && u != player &&
                ai->addUnit(u, true))
                ok = ai->followPath(u, n);
            // dropships / planes / ships (class 0x55, "Plane", FUN_1002a730) and class 0x5d craft (FUN_1002df40: S30's
            // drones and maintenance subs) aren't ground units: the port flies them through the path's points as given
            if (!ok && u >= 0 && size_t(u) < m->objects.size() && (m->objects[size_t(u)].type == 0x55 || m->objects[size_t(u)].type == 0x5d))
                if (const MissionPath* pa = m->path(n)) {
                    Flyer f;
                    for (auto& q : pa->points) f.pts.push_back(Vec3(q[0], q[1], q[2]));
                    const MissionObject& o = m->objects[size_t(u)];
                    f.pos = Vec3(float(o.pos[0]), float(o.pos[1]), float(o.pos[2]));
                    if (scene) scene->objectPosition(u, f.pos);
                    GearDef g;
                    if (o.defIndex >= 0 && size_t(o.defIndex) < m->defs.size() &&
                        parseGearDef(m->defs[size_t(o.defIndex)].data, m->defs[size_t(o.defIndex)].size, g) && g.maxRunSpeed > 1.f)
                        f.speed = g.maxRunSpeed;
                    flyers[u] = f;
                    ok = true;
                }
            if (!ok && verbose && !n.empty() && n.size() < 40)
                SDL_Log("[script] FollowPath %s for unit %d (%s) failed%s", n.c_str(), u,
                        u >= 0 && size_t(u) < m->objects.size() ? m->objects[size_t(u)].name.c_str() : "-", m->path(n) ? "" : ": no such path");
            return ok;
        }
        bool unitAssignPath(int u, const std::string& n) override {
            bool ok = ai && ai->assignPath(u, n);
            if (!ok && u >= 0 && size_t(u) < m->objects.size() && (m->objects[size_t(u)].type == 0x13e || m->objects[size_t(u)].type == 0x60))
                if (const MissionPath* pa = m->path(n)) {
                    Rail& r = rails[u];
                    const MissionObject& o = m->objects[size_t(u)];
                    Vec3 at(float(o.pos[0]), float(o.pos[1]), float(o.pos[2]));
                    if (scene) scene->objectPosition(u, at);
                    r.pts.clear();
                    for (auto& q : pa->points) r.pts.push_back(at + Vec3(q[0], q[1], q[2]) - Vec3(pa->anchor[0], pa->anchor[1], pa->anchor[2]));  // relative to the path
                    r.pos = at, r.cur = r.target = 0, r.dir = r.count = 0;
                    if (getenv("HG2_AUTODBG")) SDL_Log("rail %d on path %s (%zu points)", u, n.c_str(), r.pts.size());
                    ok = true;
                }
            if (!ok && verbose)
                SDL_Log("[script] AssignPath %s for unit %d (%s) failed%s", n.c_str(), u,
                        u >= 0 && size_t(u) < m->objects.size() ? m->objects[size_t(u)].name.c_str() : "-", m->path(n) ? "" : ": no such path");
            return ok;
        }
        bool verbose = false;
        void unitStop(int u) override { if (ai) ai->stop(u); }
        void unitSpeed(int u, float k) override {
            if (rails.count(u)) rails[u].speed = std::max(0.f, k / 3.6f);
            else if (ai) ai->speed(u, k);
        }
        void unitAttack(int u, int t) override { if (ai) ai->attack(u, t); }
        void unitNotifyRange(int u, float rr) override { if (ai) ai->notifyRange(u, rr); }
        void unitAutoPilot(int u) override { if (ai) ai->command(u, "autopilot", 0, 0, 0, std::string()); }
        void userControl(bool on) override { control = on; }
        Audio* audio = nullptr;
        void playSound(const std::string& w, bool) override { if (audio) audio->play(w, 0.9f); }
        void queueDialog(const std::string& w) override { if (audio) audio->queueDialog(w); }
        void flushDialog() override { if (audio) audio->flushDialog(); }
        Player* pl = nullptr;
        bool* externalView = nullptr;
        void unitStatus(int u, int32_t out[6]) override {
            out[0] = externalView && *externalView;
            out[1] = (pl && u == player) ? pl->postureCode() : 1;
        }
        void lockForwardStick(int u, bool on) override { if (pl && u == player) pl->lockForward(on); }
        void setHitpoints(int u, int hp) override { if (combat) combat->setHitpoints(u, float(hp)); }
        void setInvincible(int u, bool on) override { if (combat) combat->setInvincible(u, on); }
        Scene* sceneRW = nullptr;
        int unitRelation(int u, int ref) override { return ai ? ai->relation(u, ref) : -1; }
        void unitCommand(int u, const std::string& c, float a, float b, float cc, float d, const std::string& str) override {
            if (c == "water") {
                if (sceneRW) sceneRW->setWaterLevel(a);
            } else if (c == "instantDeath") {
                if (combat) combat->kill(u);
            } else if (c == "teleport") {
                if (pl && u == player) pl->teleport(Vec3(a, sceneRW ? sceneRW->groundHeight(a, cc) : b, cc));
                else if (ai) ai->teleport(u, Vec3(a, b, cc));
            } else if (c == "face") {
                if (pl && u == player) {
                    Vec3 d = Vec3(a, b, cc) - pl->position();
                    if (d.x * d.x + d.z * d.z > 0.01f) pl->setYaw(std::atan2(-d.x, -d.z));
                } else if (ai) {
                    ai->face(u, Vec3(a, b, cc));
                }
            } else if (c == "fullStop" && pl && u == player) {
                pl->halt();
            } else if ((c == "forward" || c == "backward") && rails.count(u)) {
                Rail& r = rails[u];
                if (r.pts.empty()) return;
                r.count = int(a), r.dir = c == "forward" ? 1 : -1;
                r.target = std::max(0, std::min(int(r.pts.size()) - 1, r.cur + r.dir));
            } else if (ai) {
                ai->command(u, c, a, b, cc, str, d);
            }
        }
        void destroyUnit(int u) override {  // Kill: the ordinary destruction (explosion, wreck, event 5)
            if (combat && u >= 0) combat->destroy(u);
        }
        void setSmoking(int u, int level) override {
            if (getenv("HG2_DEBUG")) SDL_Log("SetSmoking %d -> %d", u, level);
            if (combat && u >= 0) combat->setSmoking(u, level);
        }
        Vec3 windV = Vec3(6.f, 0.f, 0.f);  // the particle system's start-up wind (FUN_101246e0): (6, 0, 0), gust 1.5
        float gust = 1.5f;
        void wind(float x, float y, float z, float g) override { windV = Vec3(x, y, z), gust = g; }
        struct Strike { Vec3 p; float wait; int left; float timer = 0.5f; };
        std::vector<Strike> strikes;
        void artilleryStrike(const float p[3], float delay, int count) override {
            if (count > 0) strikes.push_back({Vec3(p[0], p[1], p[2]), delay, count});
        }
        void teamDefcon(int team, int level, int mode) override { if (ai) ai->setDefcon(team, level, mode); }
        int getTeamDefcon(int team) override { return ai ? ai->defcon(team) : 6; }
        FountainSystem* fountains = nullptr;
        void activateFountain(const std::string& n, bool on) override { if (fountains) fountains->activate(n, on); }
        std::vector<int> lightsOff;
        void activateLight(const std::string& n, bool on) override {
            for (size_t i = 0; i < m->objects.size(); i++)
                if (m->objects[i].isLight && SDL_strcasecmp(m->objects[i].name.c_str(), n.c_str()) == 0) {
                    lightsOff.erase(std::remove(lightsOff.begin(), lightsOff.end(), int(i)), lightsOff.end());
                    if (!on) lightsOff.push_back(int(i));
                }
        }
        Particles* particles = nullptr;
        void weather(int type, int level) override {  // SetWeatherType / SetWeather
            if (getenv("HG2_DEBUG")) SDL_Log("weather type %d level %d", type, level);
            if (!particles || getenv("HG2_WEATHER")) return;  // the test weather holds
            if (type >= 0) particles->setWeatherType(type);
            if (level >= 0) particles->setWeatherLevel(level);
        }
        int endCode = -1;
        float endDelay = 0;
        void missionOver(int code, float delay) override {
            if (endCode >= 0) return;
            endCode = code == 1 ? 1 : code == 2 ? 2 : 0;
            endDelay = std::max(0.f, std::min(delay, 30.f));
            SDL_Log("MissionOver(%d, %.1f)", code, delay);
        }
    } world;
    world.scene = &scene;
    world.m = &scene.mission();
    for (auto& o : scene.mission().objects) world.unitNames.push_back(o.name), world.unitTeams.push_back(o.team);
    ScriptSystem scripts(fs, world);
    scripts.verbose = T.verboseScripts;
    world.verbose = T.verboseScripts;
    scripts.load(scene.mission(), &err);
    Player player;
    bool chase = player.init(fs, scene) && !camSet;
    scene.setPlayerObject(player.object());
    player.pilot = true;
    // external camera (FUN_1007c580, the view object's +0x38): orbit +0x20, pitch +0x28 (0 level .. -30 / +80 deg),
    // zoom +0x30 in bounding radii (2 .. 8); all start at 0 / 0 / 2. The death camera (FUN_10092ff0 case 8) saves the
    // pitch and zoom and takes 45 deg at 20 radii.
    float camPitch = 0.f, camOrbit = 0;
    float camZoom = 2.f, savedPitch = 0, savedZoom = 2.f;
    bool deathCam = false;
    const float kPitchMin = -0.5235987f, kPitchMax = 1.3962632f, kCamStep = 0.043633225f;  // -30, 80, 2.5 deg
    // the targeting reticle (FUN_10117fb0, fire-control +0x23c / +0x240): -1 .. 1 on each axis, 30 deg right and
    // 20 deg up at full deflection from the torso's facing; the mouse moves it (Target ReticleX / Y), Center Torso
    // centres it. The gear's torso does not follow it: for the user's gear the torso joint only holds the sidestep's
    // opposite angle (FUN_1003c270), and the cameras do not move with it.
    float reticleX = 0.f, reticleY = 0.f;
    if (player.active() && T.startX >= 0) {
        player.teleport(Vec3(T.startX, 0, T.startZ));
        player.setYaw(T.startYaw * kDeg);
    }
    Combat combat;
    if (player.active()) combat.init(fs, scene, player.def(), player.object());
    combat.multiplayer = setup.net != nullptr;
    if (player.active() && getenv("HG2_FOOTDBG")) SDL_Log("player object %d", player.object());
    if (player.active() && !setup.player.weapons.empty()) combat.setLoadout(player.object(), setup.player.weapons);
    if (player.active() && setup.player.armor > 0) combat.setStats(player.object(), setup.player.armor, setup.player.fireControl);
    if (player.active() && setup.player.hasSystems)
        combat.setSystems(player.object(), setup.player.sensors, setup.player.maneuver, setup.player.perks, setup.player.flaws);
    AiSystem ai;
    if (player.active()) ai.init(fs, scene, combat, player);
    // the space world (DAT_1026667c + 0x14d): FUN_1008dba0 creates the world with FUN_100a3ff0(1) when the first
    // object header (0x3a / 0x3b) comes before any terrain, i.e. a mission without terrain; instant action and
    // missions started without the shell get it the same way (the briefing's space flag says the same)
    const bool space = setup.space || scene.mission().terrain.empty();
    combat.setSpace(space);
    if (space) player.space = true, ai.setSpace(true);
    if (getenv("HG2_NOZEROG") && player.active()) combat.testClearFlaws(player.object(), 1u << 14);  // test: vacuum damage
    if (space && player.active() && getenv("HG2_DEBUG"))
        SDL_Log("space: the user's gear %s Zero-G Movement (flaw word bit 14)", combat.zeroG(player.object()) ? "has" : "lacks");
    world.ai = &ai;
    // FUN_100d1e20 (the user squad's add-member, vtable 0x10148650 +0x14 over the base FUN_100cce30): gears added
    // to the user's squad get the 0.8 ejection chance (+0xd88); everything else keeps -1. The user's own gear is the
    // squad's leader (+0x470, stored by the base constructor FUN_100cbcd0 without an add), so it is never in a slot
    // and its pilot never ejects in single player.
    // the user's squad (FUN_100d2cd0 -> FUN_100d1930 for the user): its team's automatic sensor switching (+0x235)
    // goes off (FUN_100b88f0(0), the only caller of that setter; no script command reaches it)
    if (player.active()) ai.setAutoSensors(userTeam - 99, false);
    // AddToUserSquad (FUN_100d1e20 -> FUN_100cce30) puts each new member in the first empty slot scanning A0, B0, A1,
    // B1, ... (group A +0x4a4, group B +0xad4, 0x140 apart; the slot goes to brain +0x1c0, B as 4 + index), and in
    // single player nothing takes a member out (FUN_100cd040 is only called by the network code): member i is in
    // group i mod 2 at index i / 2, in the order they join - the launch record's squad order
    for (size_t i = 0; i < mates.size(); i++) {
        combat.setEjectChance(mates[i], 0.8f);
        ai.follow(mates[i], player.object(), int(i % 2), int(i / 2));
        if (!setup.squad[i].gear.weapons.empty()) combat.setLoadout(mates[i], setup.squad[i].gear.weapons);
        if (setup.squad[i].gear.armor > 0) combat.setStats(mates[i], setup.squad[i].gear.armor, setup.squad[i].gear.fireControl);
        const GearChoice& g = setup.squad[i].gear;
        if (g.hasSystems) combat.setSystems(mates[i], g.sensors, g.maneuver, g.perks, g.flaws);
    }
    Audio* audio = app.audioOk && !offscreen ? &app.audio : nullptr;
    if (audio && getenv("HG2_DEBUG")) SDL_Log("mission RedBook_Track %d", scene.mission().musicTrack);
    if (audio && scene.mission().musicTrack > 0) audio->playTrack(scene.mission().musicTrack);  // RedBook_Track
    if (audio) {
        world.audio = audio;
        combat.audio = audio;
    }
    world.combat = &combat;
    world.player = player.object();
    world.rosterOff = instant || setup.net;
    for (size_t i = 0; i < mates.size() && i < setup.squad.size(); i++) world.squadPilots.push_back({setup.squad[i].name, mates[i]});
    world.pl = &player;
    FountainSystem fountains;
    fountains.init(scene.mission());
    world.fountains = &fountains;
    // reinforcements from the mission's unit generators: a pool unit comes in at the generator, offset by its bounding
    // radius in a random direction (FUN_1005c4e0), with its template's script restarted or the generator's
    auto placeAt = [&](int idx, const Vec3& p, float yaw) {
        MissionObject& mo = scene.missionRW().objects[size_t(idx)];
        mo.pos[0] = p.x, mo.pos[1] = scene.groundHeight(p.x, p.z), mo.pos[2] = p.z, mo.rot[0] = yaw / kDeg;
    };
    FountainSystem::Host fhost;
    fhost.spawn = [&](int tpl, const Fountain& f, int reuse) -> int {
        const FountainTemplate& t = scene.mission().fountainTemplates[size_t(tpl)];
        Vec3 at(f.pos[0], f.pos[1], f.pos[2]);
        float yaw = f.rot[0];  // radians in the block
        std::string script = t.script.empty() ? f.script : t.script;
        int idx = reuse;
        if (idx < 0) {
            MissionObject o;
            o.type = t.type, o.defIndex = t.defIndex, o.team = f.team;
            o.pos[0] = at.x, o.pos[1] = at.y, o.pos[2] = at.z;
            o.rot[0] = yaw / kDeg;
            o.name = "fount" + std::to_string(scene.mission().objects.size());
            idx = scene.addObject(o);
            combat.grow(scene.mission().objects.size());
            world.unitNames.push_back(o.name), world.unitTeams.push_back(o.team);
            Vec3 p = at + FountainSystem::spawnOffset(scene.objectRadius(idx));
            placeAt(idx, p, yaw);
            ai.addUnit(idx);
        } else {
            scripts.detachScripts(idx);
            Vec3 p = at + FountainSystem::spawnOffset(scene.objectRadius(idx));
            placeAt(idx, p, yaw);
            scene.showObject(idx), scene.clearDamage(idx);
            ai.respawn(idx);
        }
        ai.teleport(idx, Vec3(scene.mission().objects[size_t(idx)].pos[0], 0, scene.mission().objects[size_t(idx)].pos[2]));
        if (!script.empty()) scripts.attachScript(script, idx);
        if (getenv("HG2_FOUNTDBG")) SDL_Log("fountain: unit %d (template %d) at %.0f,%.0f", idx, tpl, at.x, at.z);
        return idx;
    };
    fhost.recycle = [&](int o) {
        if (getenv("HG2_FOUNTDBG")) SDL_Log("fountain: unit %d recycled", o);
        scene.hideObject(o), ai.sleep(o, true);
    };
    fhost.alive = [&](int o) { return combat.alive(o) && !scene.objectHidden(o); };
    fhost.position = [&](int o, Vec3& p) { return ai.position(o, p) || scene.objectPosition(o, p); };
    fhost.inCombat = [&](int o) { return ai.mode(o) == 1; };
    fhost.findNode = [&](const Vec3& p, int& graph, int& node) { return scene.graphNodeAt(p, graph, node); };
    fhost.takeNode = [&](int graph, int node) {
        if (auto* g = const_cast<Scene::GenGraph*>(scene.walkGraph(graph)))
            if (node >= 0 && size_t(node) < g->taken.size()) g->taken[size_t(node)] = true;
    };
    fhost.onGraph = [&](int o, int graph, int node) {
        // FUN_1005c110: with links, ORDER_MOVETONODE (3, flags 0x200, 4.0) to the node's first link, brain +0x1b2 set
        const Scene::GenGraph* g = scene.walkGraph(graph);
        ai.setGraph(o, graph, node);
        if (!g || node < 0 || size_t(node) >= g->links.size() || g->links[size_t(node)].empty()) return;
        int to = g->links[size_t(node)][0];
        if (to < 0 || size_t(to) >= g->nodes.size()) return;
        ai.moveToNode(o, g->nodes[size_t(to)], to);
        if (getenv("HG2_FOUNTDBG")) SDL_Log("fountain: unit %d to graph %d node %d at %.0f,%.0f,%.0f", o, graph, to, g->nodes[size_t(to)].x,
                                            g->nodes[size_t(to)].y, g->nodes[size_t(to)].z);
    };
    Mat4 viewProj;
    bool haveView = false;
    fhost.inView = [&](const Vec3& p, float r) {
        if (!haveView) return false;
        const Mat4& vp = viewProj;
        float x = p.x * vp.m[0][0] + p.y * vp.m[1][0] + p.z * vp.m[2][0] + vp.m[3][0];
        float y = p.x * vp.m[0][1] + p.y * vp.m[1][1] + p.z * vp.m[2][1] + vp.m[3][1];
        float w = p.x * vp.m[0][3] + p.y * vp.m[1][3] + p.z * vp.m[2][3] + vp.m[3][3];
        float k = 2.f * r;
        return w > -r && std::fabs(x) <= w + k && std::fabs(y) <= w + k;
    };
    auto fountainStep = [&](float dt) {
        Vec3 back(0, 0, 1);
        if (!scene.objectAxis(player.object(), 2, back)) {
            Vec3 a0;
            AiSystem::yawAxes(player.yaw(), a0, back);
        }
        std::vector<Vec3> squad;
        for (int mo : mates) {
            Vec3 q;
            if (combat.alive(mo) && ai.position(mo, q)) squad.push_back(q);
        }
        fountains.update(dt, player.position(), back, squad, fhost);
    };
    std::string banner;
    float bannerUntil = 0;
    // instant action (FUN_10092230 for the start, FUN_10094fd0 every frame): the user goes to a random clear team-1
    // spawn point, the enemies to team-2 points (the spawn point's team is the unit's team - 99), each avoiding the
    // point the one before took (FUN_10093d10); they get a fresh brain hunting the user (FUN_100bbf40: HuntUser, no
    // dozing). A destroyed enemy comes back 2 to 3 s later (time() + 2, whole seconds) at another point of its team
    // until it has been destroyed `waves` times; all of them out: success; the user destroyed: failure (both after
    // 4 s). The list (world +0x2884) holds only the enemy fire team (FUN_10084970: launch record +0xb20 pilots, the
    // first at +0xb38, then +0xd70 + k x 0x238, team 2); the user's squad (record +0, +0x250 + k x 0x238) joins the
    // user's squad and is not on it, so mates that go down stay down.
    struct SpawnPt { int object; int team; };
    std::vector<SpawnPt> spawnPts;  // the world's spawn list (+0x2860), mission order
    for (size_t i = 0; i < scene.mission().objects.size(); i++) {
        int t = scene.mission().objects[i].type;
        if (t >= 0x56 && t <= 0x59) spawnPts.push_back({int(i), t - 0x55});
    }
    // FUN_100938b0: the n-th (1-based) point of a team, clear when no unit is inside 3 x its bounding radius
    auto spawnClear = [&](int team, int nth, int& obj) -> bool {
        obj = -1;
        for (auto& sp : spawnPts)
            if ((team == 0 || sp.team == team) && --nth == 0) {
                obj = sp.object;
                break;
            }
        if (obj < 0) return false;
        Vec3 c;
        scene.objectPosition(obj, c);
        float r = scene.objectRadius(obj), h = 3.f * (r > 0 ? r : 2.f);
        auto inside = [&](const Vec3& q) { return std::fabs(q.x - c.x) < h && std::fabs(q.z - c.z) < h && std::fabs(q.y - c.y) < h; };
        if (player.active() && inside(player.position())) return false;
        bool hit = false;
        ai.forEach([&](int o, int, const Vec3& q, bool alive) { (void)o; if (alive && inside(q)) hit = true; });
        return !hit;
    };
    // FUN_10093d10: a random point of the team (not `avoid` when there are others), else the next clear one round
    auto pickSpawn = [&](int team, int avoid, int& obj) -> int {
        int n = 0;
        for (auto& sp : spawnPts) n += team == 0 || sp.team == team;
        obj = -1;
        if (n == 0) return 0;
        int k = int(double(rand()) / (double(RAND_MAX) + 1.0) * n) + 1;
        if (k == avoid && n > 1 && ++k > n) k = 1;
        if (spawnClear(team, k, obj)) return k;
        for (int j = k;;) {
            if (++j == avoid) ++j;
            if (j > n) j = 1;
            if (j == k) break;
            if (spawnClear(team, j, obj)) return j;
        }
        obj = -1;
        return 0;
    };
    struct IaState { int deaths = 0, lastSpawn = 0xffff; bool wasAlive = true; double respawnAt = -1; };
    // FUN_10094fd0 times the comeback on the C library's time() - whole seconds: the first frame it sees the unit
    // destroyed sets +0x1a0 = time() + 2 (fild / fsubs -2.0, exact), and the unit returns on the first frame
    // time() > +0x1a0, i.e. 2 to 3 s later depending on where in the second it fell. The port keeps that clock
    // as whole seconds of the session time, offset by the wall clock's fraction of a second at the start.
    double iaPhase = 0;
    {
        SDL_Time now = 0;
        if (SDL_GetCurrentTime(&now)) iaPhase = double(now % 1000000000) * 1e-9;
    }
    auto iaClock = [&]() { return std::floor(double(scripts.time()) + iaPhase); };
    std::vector<IaState> ia(iaUnits.size());
    auto iaReset = [&](size_t i) {  // FUN_100bbf40 plus the shell's gear choices
        int idx = iaUnits[i];
        const GearChoice& eg = setup.enemies[i].gear;
        if (!eg.weapons.empty()) combat.setLoadout(idx, eg.weapons);
        if (eg.armor > 0) combat.setStats(idx, eg.armor, eg.fireControl);
        if (eg.hasSystems) combat.setSystems(idx, eg.sensors, eg.maneuver, eg.perks, eg.flaws);
        ai.command(idx, "nonCombatTactic", 0, 0, 0, "HuntUser");
        ai.setAutoSleep(idx, false);
    };
    auto iaPlace = [&](size_t i) -> bool {
        int idx = iaUnits[i], obj = -1;
        int k = pickSpawn(scene.mission().objects[size_t(idx)].team - 99, ia[i].lastSpawn, obj);
        ia[i].lastSpawn = k;
        if (!k || obj < 0) return false;
        Vec3 p;
        scene.objectPosition(obj, p);
        placeAt(idx, p, float(scene.mission().objects[size_t(obj)].rot[0]) * kDeg);
        return true;
    };
    if (instant) {
        int obj = -1;
        if (player.active() && pickSpawn(userTeam - 99, 0xffff, obj) && obj >= 0) {
            Vec3 p;
            scene.objectPosition(obj, p);
            float yaw = float(scene.mission().objects[size_t(obj)].rot[0]) * kDeg;
            player.teleport(Vec3(p.x, 0, p.z)), player.setYaw(yaw);
            Vec3 a0, a2;
            AiSystem::yawAxes(yaw, a0, a2);
            for (size_t i = 0; i < mates.size(); i++) {  // the squad formed round the user (FUN_100cc8f0)
                Vec3 q = AiSystem::formationPoint(int(i % 2), int(i / 2), player.position(), a0, a2);
                ai.teleport(mates[i], q), ai.face(mates[i], q - a2 * 10.f);
            }
        }
        int prev = 0xffff;
        for (size_t i = 0; i < iaUnits.size(); i++) {
            ia[i].lastSpawn = prev;
            if (iaPlace(i)) {
                const MissionObject& mo = scene.mission().objects[size_t(iaUnits[i])];
                Vec3 q(float(mo.pos[0]), 0, float(mo.pos[2])), a0, a2;
                AiSystem::yawAxes(float(mo.rot[0]) * kDeg, a0, a2);
                ai.teleport(iaUnits[i], q), ai.face(iaUnits[i], q - a2 * 10.f);
            }
            prev = ia[i].lastSpawn;
            iaReset(i);
        }
        SDL_Log("instant action: %zu enemies, %d lives each, %zu spawn points", iaUnits.size(), setup.waves, spawnPts.size());
    }
    auto iaStep = [&]() {
        if (!instant || world.endCode >= 0) return;
        if (player.active() && !combat.alive(player.object())) {
            world.missionOver(0, 4.f);
            return;
        }
        bool done = true;
        for (size_t i = 0; i < iaUnits.size(); i++) {
            int idx = iaUnits[i];
            bool alive = combat.alive(idx);
            if (!alive && ia[i].wasAlive) ia[i].deaths++;  // FUN_100f95d0: +0x199
            ia[i].wasAlive = alive;
            if (alive) {
                done = false;
                continue;
            }
            if (ia[i].deaths >= setup.waves) continue;
            done = false;
            if (ia[i].respawnAt < 0) {
                ia[i].respawnAt = iaClock() + 2.0;
            } else if (iaClock() > ia[i].respawnAt) {
                ia[i].respawnAt = -1;
                iaPlace(i);  // no clear point: it comes back where it fell
                scene.clearDamage(idx);
                ai.respawn(idx);
                const MissionObject& mo = scene.mission().objects[size_t(idx)];
                Vec3 q(float(mo.pos[0]), 0, float(mo.pos[2])), a0, a2;
                AiSystem::yawAxes(float(mo.rot[0]) * kDeg, a0, a2);
                ai.teleport(idx, q), ai.face(idx, q - a2 * 10.f);
                iaReset(i);
                ia[i].wasAlive = true;
                if (getenv("HG2_DEBUG")) SDL_Log("instant action: enemy %d back (%d/%d) at spawn %d", idx, ia[i].deaths, setup.waves, ia[i].lastSpawn);
            }
        }
        if (done) world.missionOver(1, 4.f);
    };
    // ambient sound objects: buildings / props whose definition names a looping sound (Sounds.lst EAR_PERF_LOOP).
    // Every object plays its own instance (FUN_1012f608 makes one EAR event per object), at EAR's level for its
    // distance; looping sounds share EAR channel 2, registered with 16 voices at the high sound setting
    // (FUN_1012e708 -> RegisterChannel(2, 0x10, .., 16 << 16)). Which voices EAR keeps when more are due is not
    // decoded: the port keeps the 16 loudest.
    struct Ambient { int object; std::string wav; int handle = -1; };
    std::vector<Ambient> ambients;
    if (audio) {
        const Mission& mm = scene.mission();
        for (size_t i = 0; i < mm.objects.size(); i++) {
            const auto& o = mm.objects[i];
            if (int(i) == player.object() || o.defIndex < 0 || size_t(o.defIndex) >= mm.defs.size()) continue;
            if (o.type == 0x48 || o.type == 0x4e || o.type == 0x4f || o.type == 0x4b) continue;  // units have engines
            GearDef g;
            if (!parseGearDef(mm.defs[size_t(o.defIndex)].data, mm.defs[size_t(o.defIndex)].size, g) || g.soundMotor.empty()) continue;
            const Audio::SoundInfo* si = audio->info(g.soundMotor);
            if (si && si->loop) ambients.push_back({int(i), g.soundMotor});
        }
        if (getenv("HG2_DEBUG")) {
            std::map<std::string, int> n;
            for (auto& a : ambients) n[a.wav]++;
            for (auto& [w, k] : n) SDL_Log("ambient %s x%d", w.c_str(), k);
        }
    }
    auto updateAmbient = [&]() {
        std::vector<std::pair<float, Ambient*>> due;
        for (auto& a : ambients) {
            Vec3 p;
            if (scene.objectHidden(a.object) || !scene.objectPosition(a.object, p)) continue;
            float lv = audio->loopLevel(a.wav, p);
            if (lv > 0) due.push_back({lv, &a});
        }
        std::sort(due.begin(), due.end(), [](const auto& x, const auto& y) { return x.first > y.first; });
        if (due.size() > 16) due.resize(16);
        for (auto& a : ambients) {
            bool on = std::any_of(due.begin(), due.end(), [&](const auto& d) { return d.second == &a; });
            Vec3 p;
            if (on && a.handle < 0) a.handle = audio->startLoop(a.wav, 0.f);
            if (!on && a.handle >= 0) audio->stopLoop(a.handle), a.handle = -1;
            if (a.handle >= 0 && scene.objectPosition(a.object, p)) audio->setLoop(a.handle, 1.f, &p);
        }
    };
    Particles particles;
    // the particle setting picks the original's detail (DAT_101fcf44 +0xb0: 0 low, 1 high, 2 medium)
    particles.init(fs, r, app.settings.particles <= 0 ? 0 : app.settings.particles == 1 ? 2 : 1);
    particles.groundHeight = [&](float x, float z) { return scene.groundHeight(x, z); };
    particles.groundNormal = [&](float x, float z) { return scene.groundNormal(x, z); };
    world.particles = &particles;
    if (const char* w = getenv("HG2_WEATHER")) {  // test: "type:level", e.g. 1:3
        particles.setWeatherType(atoi(w));
        particles.setWeatherLevel(atoi(strchr(w, ':') ? strchr(w, ':') + 1 : "3"));
    }
    // weather sounds: the rain loop (FUN_100f7560: Envrain2.wav for the light levels, Envrain1.wav for the heavy)
    // and the thunder at each lightning strike (FUN_100f7c40, envthnd1.wav)
    int rainLoop = -1, rainLoopKind = -1;
    particles.onLightning = [&](const Vec3& at) {
        if (getenv("HG2_DEBUG")) SDL_Log("lightning at %.0f,%.0f", at.x, at.z);
        if (audio) audio->play("envthnd1.wav", 1.f, &at);
    };
    auto weatherSound = [&]() {
        int k = particles.rainSound();
        if (!audio || k == rainLoopKind) return;
        if (rainLoop >= 0) audio->stopLoop(rainLoop), rainLoop = -1;
        rainLoopKind = k;
        if (k < 0) return;
        const char* wav = k == 0 ? "Envrain2.wav" : "Envrain1.wav";
        rainLoop = audio->startLoop(wav, 1.f);
        if (getenv("HG2_DEBUG")) SDL_Log("rain loop %s", wav);
    };
    combat.particles = &particles;
    player.particles = &particles;
    ai.setParticles(&particles);
    world.sceneRW = &scene;
    // ArtilleryStrike (the "Artillery" object, FUN_100348b0 / FUN_100349d0 / FUN_10034a50): once the delay has run
    // a 0.5 s timer starts; each time it runs out a shell is released at rest 150 m above the script's point,
    // offset by 0..30 m (-15) in x and z, until `count` have gone
    auto updateStrikes = [&](float dt) {
        for (auto& st : world.strikes) {
            if (st.left <= 0 || (st.wait -= dt) > 0) continue;
            if ((st.timer -= dt) >= 0) continue;
            float x = st.p.x + float(rand() % 30000) * 0.001f - 15.f, z = st.p.z + float(rand() % 30000) * 0.001f - 15.f;
            combat.artilleryShell(Vec3(x, st.p.y + 150.f, z));
            st.left--, st.timer = 0.5f;
        }
        world.strikes.erase(std::remove_if(world.strikes.begin(), world.strikes.end(), [](const World::Strike& st) { return st.left <= 0; }),
                            world.strikes.end());
    };
    ai.onEvent = [&](int obj, int ev) { scripts.unitSignal(obj, ev); };
    // engine events from combat: hits (11, 12 while the user shoots), damaged (2), destroyed (5)
    std::map<int, float> lastHitEvent;
    auto drainCombatEvents = [&]() {
        for (auto& n : combat.nearShots) ai.nearMiss(n.victim, n.shooter);
        combat.nearShots.clear();
        for (auto& h : combat.hits) {
            ai.hitBy(h.victim, h.shooter);
            if (h.level >= 2) {  // flinch (light hits only now and then)
                if (h.victim == player.object()) player.hitReaction(h.level);
                else if (h.level >= 3 || rand() % 3 == 0) ai.hitReaction(h.victim, h.level);
            }
            float& last = lastHitEvent[h.victim];
            if (scripts.time() - last < 0.5f && last > 0) continue;
            last = scripts.time();
            scripts.unitSignal(h.victim, EV_HIT);
            if (h.shooter == player.object()) scripts.unitSignal(h.victim, EV_HIT_BY_USER);
            scripts.unitSignal(h.victim, h.level);  // 2 light, 3 medium, 4 heavy damage (FUN_10058fd0)
        }
        combat.hits.clear();
        combat.damaged.clear();
        for (int obj : combat.destroyed) {
            scripts.unitSignal(obj, EV_DESTROYED);
            for (size_t i = 0; i < mates.size(); i++)
                if (mates[i] == obj) {
                    if (combat.ejected(obj)) {  // FUN_10062980: the pilot got out (squad slots: 0.8 chance)
                        banner = setup.squad[i].name + " EJECTED", bannerUntil = scripts.time() + 3.f;
                    } else {
                        res.squadLost.push_back(setup.squad[i].name);
                        banner = setup.squad[i].name + " IS DOWN", bannerUntil = scripts.time() + 3.f;
                    }
                }
        }
        combat.destroyed.clear();
        combat.ejectedList.clear();
    };
    world.externalView = &external;
    if (audio) {
        player.audio = audio, player.engineLoop = true;
        ai.setAudio(audio);
    }
    // player-attached camera: cockpit (internal) or external
    auto viewCamera = [&](Vec3& eye, Vec3& at) {
        if (external) {
            player.chaseCamera(eye, at, camPitch, camOrbit, camZoom);
            // FUN_1007c070: 16 samples from the sphere centre to the eye, each wanting the ground below it by a
            // clearance growing 0.1375 x the view's near half-height (taken as 0.75) a sample; the eye is raised by the
            // worst shortfall, looks back at the centre, and the pitch becomes the new tilt (it stays raised)
            Vec3 step = (eye - at) * 0.0625f;
            float worst = 0, clear = 0;
            Vec3 p = at;
            for (int i = 0; i < 16; i++, p = p + step, clear += 0.75f * 0.1375f) {
                float m = p.y - scene.groundHeight(p.x, p.z) - clear;
                if (m < worst) worst = m;
            }
            if (worst < 0) {
                eye.y -= worst;
                Vec3 d = eye - at;
                float len = length(d);
                if (len > 1e-3f) camPitch = std::acos(std::min(1.f, std::sqrt(d.x * d.x + d.z * d.z) / len));
            }
        } else {
            // the cockpit view is the model's pov frame (FUN_10083d30 attaches the internal camera to "POV" / "pov"),
            // which turns with the torso; the cockpit camera takes no keys (its key slot is a no-op)
            player.cockpitCamera(eye, at, 0.f, player.twist - player.legAngle());
            Vec3 pov;  // the model's pilot eye frame follows posture and walk bob
            if (scene.frameWorld(player.object(), "pov", pov)) at += pov - eye, eye = pov;
        }
    };
    auto aimPoint = [&](Vec3& muzzle, Vec3& aim) {
        // the reticle's direction (FUN_10117fb0: yaw = 30 x reticle X, pitch = 20 x reticle Y degrees, stored on the
        // weapon at +0x1e8 / +0x1ec) from the pilot's eye (the model's pov frame), whatever the view
        Vec3 eye, at;
        float facing = player.yaw() + player.twist - player.legAngle();
        player.cockpitCamera(eye, at, 0.f, player.twist - player.legAngle());
        Vec3 pov;
        if (scene.frameWorld(player.object(), "pov", pov)) eye = pov;
        float yaw = facing - std::clamp(reticleX, -1.f, 1.f) * 30.f * kDeg, pitch = std::clamp(reticleY, -1.f, 1.f) * 20.f * kDeg;
        Vec3 dir(-std::sin(yaw) * std::cos(pitch), std::sin(pitch), -std::cos(yaw) * std::cos(pitch));
        if (!combat.muzzle(player.object(), muzzle))
            muzzle = player.position() + Vec3(0, 5.f, 0) + Vec3(-std::sin(player.yaw()), 0, -std::cos(player.yaw())) * 2.f;
        // shots converge where the reticle's line meets the ground, else 300 m out (the port's convergence range)
        const float reach = 300.f;
        float f = scene.terrainRay(eye, eye + dir * reach);
        aim = eye + dir * (reach * std::clamp(f, 0.05f, 1.f));
    };
    if (!camSet) {
        cam.pos = scene.startPosition() + Vec3(0, 12, 0);
        cam.pos.y = std::max(cam.pos.y, scene.groundHeight(cam.pos.x, cam.pos.z) + 10);
    }
    bool sniperZoom = false;
    auto frameParams = [&]() {
        FrameParams fp = scene.frameDefaults();
        float aspect = float(r.width() > 0 ? r.width() : width) / float(r.height() > 0 ? r.height() : height);
        fp.eye = cam.pos;
        if (T.noFog) fp.fogIntensity = 0;
        float ce[3], ca[3];
        if (scripts.cinematicCamera(ce, ca)) {  // scripted camera path
            Vec3 eye(ce[0], ce[1], ce[2]), at(ca[0], ca[1], ca[2]);
            eye.y = std::max(eye.y, scene.groundHeight(eye.x, eye.z) + 1.5f);
            fp.eye = eye;
            fp.view = Mat4::lookAt(eye, at, Vec3(0, 1, 0));
        } else if (chase) {
            Vec3 eye, at;
            viewCamera(eye, at);
            fp.eye = eye;
            fp.view = Mat4::lookAt(eye, at, Vec3(0, 1, 0));
        } else {
            fp.view = Mat4::lookAt(cam.pos, cam.pos + cam.forward(), Vec3(0, 1, 0));
        }
        fp.proj = Mat4::perspective((sniperZoom ? 20.f : 60.f) * kDeg, aspect, 6000.f, 0.5f);  // near/far swapped: reversed Z
        return fp;
    };
    Hud hud;
    float missionClock = 0;
    std::string damageMsg;
    float damageMsgUntil = 0;
    bool paused = false;
    int pauseHover = -1;
    float pauseMouseX = -1, pauseMouseY = -1;
    static const char* kPauseItems[] = {"RESUME MISSION", "ABORT MISSION", "QUIT GAME"};
    auto pauseItemRect = [&](int i, float& x, float& y, float& w, float& h) {
        int W = r.width() > 0 ? r.width() : width, H = r.height() > 0 ? r.height() : height;
        float s = std::max(1.f, float(H) / 720.f);
        w = 320 * s, h = 40 * s, x = W * 0.5f - w * 0.5f, y = H * 0.42f + float(i) * 52 * s;
    };
    // the user's target (fire-control +0x1d0): kept until changed by the targeting keys (FUN_10114210): Target At
    // Reticle (the hostile under the reticle; here the nearest within 10 degrees of its line, 800 m, in sight), Target
    // Cycle / Previous Target (through the detected hostiles), Nearest Enemy, Target Friendly. Squad commands (Attack
    // / Defend My Target) use it, the HUD brackets it. It is dropped while the radar is jammed and when the unit is
    // destroyed or no longer detected.
    int userTarget = -1, targetIdx = -1;
    auto targetable = [&](int i, bool friendly) {
        const auto& objs = scene.mission().objects;
        if (i == player.object() || i < 0 || size_t(i) >= objs.size()) return false;
        const auto& o = objs[size_t(i)];
        int ptm = objs[size_t(player.object())].team;
        if (!isUnitObjectType(o.type) || !combat.alive(i) || scene.objectHidden(i) || o.team < 100) return false;
        if (friendly) return o.team == ptm;
        // only contacts on the user's radar (the targeting FUN_10114210 cycles the radar's contact lists)
        return o.team != ptm && combat.detectedBy(i, ptm);
    };
    auto contacts = [&](bool friendly) {
        std::vector<std::pair<float, int>> l;
        const auto& objs = scene.mission().objects;
        for (size_t i = 0; i < objs.size(); i++) {
            Vec3 p;
            if (!targetable(int(i), friendly) || !scene.objectPosition(int(i), p)) continue;
            l.push_back({length(p - player.position()), int(i)});
        }
        std::sort(l.begin(), l.end());
        return l;
    };
    auto pickTarget = [&]() {  // Target At Reticle
        if (!player.active() || !combat.alive(player.object())) return;
        if (combat.radarJammed(player.object())) return;
        Vec3 mz, aim;
        aimPoint(mz, aim);
        Vec3 eye = player.position() + Vec3(0, 5, 0), dir = normalize(aim - eye);
        float best = std::cos(10.f * kDeg);
        const auto& objs = scene.mission().objects;
        for (size_t i = 0; i < objs.size(); i++) {
            if (!targetable(int(i), false)) continue;
            Vec3 p;
            if (!scene.objectPosition(int(i), p)) continue;
            p.y += 3.f;
            Vec3 d = p - eye;
            float dl = length(d);
            if (dl < 5.f || dl > 800.f) continue;
            float c = dot(d * (1.f / dl), dir);
            if (c > best && scene.terrainRay(eye, p) > 0.98f) best = c, userTarget = int(i);
        }
    };
    auto cycleTarget = [&](int step, bool friendly) {  // Target Cycle (+1) / Previous Target (-1); 0: the first (nearest)
        if (!player.active() || combat.radarJammed(player.object())) return;
        auto l = contacts(friendly);
        if (l.empty()) return;
        int n = int(l.size());
        if (step == 0) targetIdx = 0;
        else if (targetIdx < 0) targetIdx = step > 0 ? 0 : n - 1;
        else targetIdx = ((targetIdx + step) % n + n) % n;
        userTarget = l[size_t(targetIdx)].second;
    };
    auto validateTarget = [&]() {
        if (userTarget < 0) return;
        bool friendly = scene.mission().objects[size_t(userTarget)].team == scene.mission().objects[size_t(player.object())].team;
        if (!player.active() || !combat.alive(player.object()) || combat.radarJammed(player.object()) || !targetable(userTarget, friendly))
            userTarget = -1, targetIdx = -1;
    };
    // squad commands (Defaults.dft: Ctrl+T attack my target, Ctrl+D defend my target, Ctrl+S hold ground, Ctrl+R
    // regroup, Ctrl+A fire at will, Ctrl+G go to the selected nav point)
    auto issueOrder = [&](int k) {
        if (getenv("HG2_TACDBG")) SDL_Log("issueOrder %d squad %d", k, ai.squadSize(player.object()));
        if (ai.squadSize(player.object()) <= 0 || k < 0 || k > 5) return;
        if (!combat.commsOk(player.object())) {
            banner = "COMMS DOWN", bannerUntil = scripts.time() + 1.5f;
            return;
        }
        static const char* names[] = {"ATTACK MY TARGET", "DEFEND MY TARGET", "HOLD GROUND", "REGROUP", "FIRE AT WILL", "GO TO NAV POINT"};
        int order = k == 0 ? AiSystem::OrderAttack : k == 1 ? AiSystem::OrderDefend : k == 2 ? AiSystem::OrderHold
                  : k == 3 ? AiSystem::OrderRegroup : k == 4 ? AiSystem::OrderFireAtWill : AiSystem::OrderGoto;
        int obj = -1;
        Vec3 pos = player.position();
        bool ok = true;
        if (order == AiSystem::OrderAttack) obj = userTarget, ok = obj >= 0;
        if (order == AiSystem::OrderDefend) obj = userTarget >= 0 ? userTarget : player.object();
        if (order == AiSystem::OrderGoto) {
            int cp = scripts.selectedCheckpoint;
            ok = cp >= 0;
            if (ok) pos = Vec3(scripts.checkpoints[cp].pos[0], 0, scripts.checkpoints[cp].pos[2]);
        }
        if (ok) {
            ai.squadOrder(player.object(), order, obj, pos);
            banner = std::string("SQUAD: ") + names[k], bannerUntil = scripts.time() + 2.f;
            if (audio) audio->play("general.wav", 0.6f);
        } else {
            banner = order == AiSystem::OrderGoto ? "NO NAV POINT SELECTED" : "NO TARGET", bannerUntil = scripts.time() + 1.5f;
        }
        if (getenv("HG2_TACDBG")) SDL_Log("squad order %d: %s (%.0f,%.0f)", order, banner.c_str(), pos.x, pos.z);
    };
    auto updateFlyers = [&](float dt) {
        for (auto it = world.flyers.begin(); it != world.flyers.end();) {
            auto& f = it->second;
            float step = f.speed * dt;
            while (f.idx < f.pts.size() && step > 0) {
                Vec3 d = f.pts[f.idx] - f.pos;
                float len = length(d);
                if (len > 0.01f) f.yaw = std::atan2(-d.x, -d.z);
                if (len <= step) f.pos = f.pts[f.idx++], step -= len;
                else f.pos += d * (step / len), step = 0;
            }
            scene.setObjectPose(it->first, f.pos, f.yaw, -1, 0.f);
            if (f.idx >= f.pts.size()) it = world.flyers.erase(it);
            else ++it;
        }
        for (auto& [obj, r] : world.rails) {
            if (r.dir == 0) continue;
            float step = r.speed * std::min(dt, 0.06f);  // elevators clamp the step to 0.06 s (FUN_100740a0)
            Vec3 d = r.pts[size_t(r.target)] - r.pos;
            float len = length(d);
            if (len <= step) {
                r.pos = r.pts[size_t(r.target)], r.cur = r.target;
                int next = r.target + r.dir;
                if (--r.count < 1 || next < 0 || next >= int(r.pts.size())) r.dir = 0;
                else r.target = next;
            } else {
                r.pos += d * (step / len);
                if (d.x * d.x + d.z * d.z > 0.01f) r.yaw = std::atan2(-d.x, -d.z);
            }
            scene.setObjectPose(obj, r.pos, r.yaw, -1, 0.f);
            if (getenv("HG2_AUTODBG")) SDL_Log("rail %d: point %d -> %d (%d left) at %.0f,%.0f,%.0f", obj, r.cur, r.target, r.count, r.pos.x, r.pos.y, r.pos.z);
        }
    };
    // the user's gear on a scripted path (see World::unitFollowPath): steer at the next point, walk, and drop the
    // order once the user has the controls back
    bool userAutoWas = false;
    auto userAutopilot = [&](PlayerInput& pin, bool dead) {
        bool scripted = scripts.cinematic.active || scripts.cinematicMode || !world.control;
        if (world.userPath.empty() || dead || !player.active()) return userAutoWas = false, void();
        if (!scripted) {
            if (userAutoWas) world.userPath.clear();
            userAutoWas = false;
            return;
        }
        userAutoWas = true;
        Vec3 pos = player.position();
        while (world.userPathIdx < world.userPath.size()) {
            Vec3 d = world.userPath[world.userPathIdx] - pos;
            if (d.x * d.x + d.z * d.z > 16.f) break;
            world.userPathIdx++;
        }
        if (world.userPathIdx >= world.userPath.size()) {
            world.userPath.clear();
            return;
        }
        Vec3 d = world.userPath[world.userPathIdx] - pos;
        float want = std::atan2(-d.x, -d.z), err = want - player.yaw();
        while (err > 3.14159265f) err -= 6.2831853f;
        while (err < -3.14159265f) err += 6.2831853f;
        pin = PlayerInput{};
        pin.turn = std::max(-1.f, std::min(1.f, err * 2.f));
        pin.throttle = std::fabs(err) < 1.2f ? 1.f : 0.f;
        if (getenv("HG2_AUTODBG")) SDL_Log("user autopilot: point %zu/%zu, %.0f m, turn %.2f", world.userPathIdx, world.userPath.size(), std::sqrt(d.x * d.x + d.z * d.z), pin.turn);
    };
    auto buildHud = [&](const FrameParams& fp) {
        int W = r.width() > 0 ? r.width() : width, H = r.height() > 0 ? r.height() : height;
        hud.begin(W, H);
        float s = std::max(1.f, float(H) / 720.f);
        const uint32_t green = 0x7cff8cffu, amber = 0xffc040ffu, red = 0xff4030ffu, dim = 0x00000090u;
        bool cine = scripts.cinematic.active || scripts.cinematicMode;
        if (cine) {  // letterbox during scripted camera work
            hud.rect(0, 0, float(W), H * 0.1f, 0x000000ffu), hud.rect(0, H * 0.9f, float(W), H * 0.1f + 1, 0x000000ffu);
        }
        if (player.active() && chase && !cine) {
            // aim reticle: project the aim point
            Vec3 mz, aim;
            aimPoint(mz, aim);
            Mat4 vp = fp.view * fp.proj;
            float x = aim.x * vp.m[0][0] + aim.y * vp.m[1][0] + aim.z * vp.m[2][0] + vp.m[3][0];
            float y = aim.x * vp.m[0][1] + aim.y * vp.m[1][1] + aim.z * vp.m[2][1] + vp.m[3][1];
            float w = aim.x * vp.m[0][3] + aim.y * vp.m[1][3] + aim.z * vp.m[2][3] + vp.m[3][3];
            if (w > 0.1f) {
                float sx = (x / w * 0.5f + 0.5f) * W, sy = (0.5f - y / w * 0.5f) * H, c = 9 * s;
                hud.rect(sx - c, sy - s, c * 0.7f, 2 * s, green), hud.rect(sx + c * 0.3f, sy - s, c * 0.7f, 2 * s, green);
                hud.rect(sx - s, sy - c, 2 * s, c * 0.7f, green), hud.rect(sx - s, sy + c * 0.3f, 2 * s, c * 0.7f, green);
            }
            Vec3 tq;
            if (userTarget >= 0 && scene.objectPosition(userTarget, tq)) {  // target bracket
                tq.y += 3.f;
                float tx = tq.x * vp.m[0][0] + tq.y * vp.m[1][0] + tq.z * vp.m[2][0] + vp.m[3][0];
                float ty = tq.x * vp.m[0][1] + tq.y * vp.m[1][1] + tq.z * vp.m[2][1] + vp.m[3][1];
                float tw = tq.x * vp.m[0][3] + tq.y * vp.m[1][3] + tq.z * vp.m[2][3] + vp.m[3][3];
                if (tw > 0.1f) {
                    float sx = (tx / tw * 0.5f + 0.5f) * W, sy = (0.5f - ty / tw * 0.5f) * H, b = 18 * s, t = 2 * s, l = 7 * s;
                    for (int cx : {-1, 1})
                        for (int cy : {-1, 1}) {
                            float ex = sx + float(cx) * b, ey = sy + float(cy) * b;
                            hud.rect(cx < 0 ? ex : ex - l, ey - t * 0.5f, l, t, red);
                            hud.rect(ex - t * 0.5f, cy < 0 ? ey : ey - l, t, l, red);
                        }
                    char tb[48];
                    snprintf(tb, sizeof tb, "%.0f m", length(tq - player.position()));
                    hud.text(sx + b + 4 * s, sy - 6 * s, tb, red, 0.8f * s);
                }
            }
            // status panel
            float px = 16 * s, py = H - 96 * s;
            hud.rect(px - 6 * s, py - 6 * s, 380 * s, 88 * s, dim);
            {  // damage diagram: head, torso, arms, legs coloured by the weakest part in each region
                auto col = [&](char rg) {
                    float h = combat.regionHealth(player.object(), rg);
                    return h <= 0 ? 0x404040ffu : h > 0.6f ? green : h > 0.3f ? amber : red;
                };
                float dx = px + 312 * s, dy = py - 2 * s, u = 6 * s;
                hud.rect(dx + 3 * u, dy, 2 * u, 2 * u, col('H'));
                hud.rect(dx + 2.5f * u, dy + 2.3f * u, 3 * u, 4 * u, col('T'));
                hud.rect(dx + 0.8f * u, dy + 2.3f * u, 1.4f * u, 4 * u, col('A'));
                hud.rect(dx + 5.8f * u, dy + 2.3f * u, 1.4f * u, 4 * u, col('a'));
                hud.rect(dx + 2.5f * u, dy + 6.6f * u, 1.3f * u, 5 * u, col('L'));
                hud.rect(dx + 4.2f * u, dy + 6.6f * u, 1.3f * u, 5 * u, col('l'));
            }
            float hp = combat.health(player.object());
            hud.text(px, py, "ARMOUR", green, s);
            hud.frame(px + 80 * s, py + 2 * s, 200 * s, 14 * s, green, s);
            hud.rect(px + 82 * s, py + 4 * s, 196 * s * hp, 10 * s, hp > 0.5f ? green : hp > 0.25f ? amber : red);
            std::string wl = combat.status();
            size_t cut = wl.find("  ");
            hud.text(px, py + 24 * s, cut == std::string::npos ? wl : wl.substr(cut + 2), amber, s);
            char buf[96];
            static const char* posture[] = {"PRONE", "STAND", "KNEEL", "SMS"};
            snprintf(buf, sizeof buf, "%3.0f km/h  %-5s  %s", std::fabs(player.speed()) * 3.6f, posture[player.postureCode() & 3],
                     external ? "EXT" : "INT");
            hud.text(px, py + 48 * s, buf, green, s);
            // radar (FUN_101117f0 / FUN_10111c50): the contacts the user's team has detected within the radar's
            // 300 m (+0x730); static when jammed, nothing once the radar is knocked out
            float R = 70 * s, rx = W - R - 16 * s, ry = R + 16 * s;
            const float range = 300.f;
            bool sensors = combat.sensorsOk(player.object()), jam = combat.radarJammed(player.object());
            hud.rect(rx - R, ry - R, 2 * R, 2 * R, dim);
            bool blink = combat.sensorsDamaged(player.object()) && std::fmod(missionClock, 1.f) < 0.5f;
            hud.frame(rx - R, ry - R, 2 * R, 2 * R, blink ? red : green, s);
            hud.rect(rx - 2 * s, ry - 2 * s, 4 * s, 4 * s, green);
            int ptm = scene.mission().objects[size_t(player.object())].team;
            float yw = player.yaw(), cy = std::cos(yw), sy2 = std::sin(yw);
            if (!sensors) hud.text(rx - R + 8 * s, ry - 8 * s, "RADAR DOWN", red, s);
            else if (jam) {
                for (int n = 0; n < 40; n++)
                    hud.rect(rx - R + float(rand() % int(2 * R - 4 * s)), ry - R + float(rand() % int(2 * R - 4 * s)), 3 * s, 2 * s, green);
                hud.text(rx - R + 8 * s, ry - 8 * s, "JAMMED", amber, s);
            }
            // sensor mode, ECM and how visible the user is (+0x76c: the user's signal)
            snprintf(buf, sizeof buf, "%s%s", combat.activeSensors(player.object()) ? "ACTIVE" : "PASSIVE",
                     combat.ecmOn(player.object()) ? "  ECM" : "");
            hud.text(rx - R + 4 * s, ry - R + 4 * s, buf, combat.ecmOn(player.object()) ? amber : green, 0.7f * s);
            hud.frame(rx + R - 44 * s, ry - R + 5 * s, 40 * s, 8 * s, green, s);
            hud.rect(rx + R - 43 * s, ry - R + 6 * s, 38 * s * std::min(1.f, float(combat.signal(player.object()))), 6 * s, amber);
            int hostiles = 0;
            ai.forEach([&](int o, int team, const Vec3& p, bool alive) {
                if (alive && team != ptm) hostiles++;
                if (!alive || !sensors || jam || !combat.detectedBy(o, ptm)) return;
                Vec3 d = p - player.position();
                float fx = -d.x * sy2 - d.z * cy, rxl = d.z * sy2 - d.x * cy;  // screen right = (-cos, 0, sin)
                float u = rxl / range * R, v = -fx / range * R;
                if (u * u + v * v > (R - 3 * s) * (R - 3 * s) || std::fabs(u) > R - 3 * s || std::fabs(v) > R - 3 * s) return;
                hud.rect(rx + u - 2 * s, ry + v - 2 * s, 4 * s, 4 * s, team == ptm ? green : red);
            });
            snprintf(buf, sizeof buf, "HOSTILES %d", hostiles);
            hud.text(rx - R, ry + R + 4 * s, buf, red, s);
            float np[3];
            if (scripts.checkpointPosition(scripts.selectedCheckpoint, np)) {
                Vec3 nav(np[0], scene.groundHeight(np[0], np[2]), np[2]);
                Vec3 d = nav - player.position();
                float fx = -d.x * sy2 - d.z * cy, rxl = d.z * sy2 - d.x * cy;
                float u = rxl / range * R, v = -fx / range * R, mm = std::max(std::fabs(u), std::fabs(v));
                if (mm > R - 4 * s) u *= (R - 4 * s) / mm, v *= (R - 4 * s) / mm;
                hud.frame(rx + u - 4 * s, ry + v - 4 * s, 8 * s, 8 * s, amber, s);
                const std::string& label = scripts.checkpoints[size_t(scripts.selectedCheckpoint)].label;
                float dist = std::sqrt(d.x * d.x + d.z * d.z);
                snprintf(buf, sizeof buf, "%s  %.0f m", label.c_str(), dist);
                hud.text(std::min(rx - R, W - hud.textWidth(buf, s) - 8 * s), ry + R + 24 * s, buf, amber, s);
                Vec3 mk = nav + Vec3(0, 6, 0);
                Mat4 vp2 = fp.view * fp.proj;
                float x = mk.x * vp2.m[0][0] + mk.y * vp2.m[1][0] + mk.z * vp2.m[2][0] + vp2.m[3][0];
                float y = mk.x * vp2.m[0][1] + mk.y * vp2.m[1][1] + mk.z * vp2.m[2][1] + vp2.m[3][1];
                float w = mk.x * vp2.m[0][3] + mk.y * vp2.m[1][3] + mk.z * vp2.m[2][3] + vp2.m[3][3];
                if (w > 0.1f) {
                    float sx = (x / w * 0.5f + 0.5f) * W, sy = (0.5f - y / w * 0.5f) * H;
                    if (sx > 0 && sx < W && sy > 0 && sy < H) {
                        float c = 8 * s;
                        hud.frame(sx - c, sy - c, 2 * c, 2 * c, amber, 2 * s);
                        hud.text(sx - hud.textWidth(label, 0.8f * s) * 0.5f, sy + c + 2 * s, label, amber, 0.8f * s);
                    }
                }
            }
            // squad mates: name and armour
            for (size_t i = 0; i < mates.size(); i++) {
                float h = combat.health(mates[i]);
                float qy = ry + R + 52 * s + float(i) * 18 * s;
                std::string nm = setup.squad[i].name;
                hud.text(rx - R, qy, nm, h > 0 ? green : red, 0.8f * s);
                hud.frame(rx - R + 90 * s, qy + 2 * s, 50 * s, 8 * s, green, s);
                if (h > 0) hud.rect(rx - R + 91 * s, qy + 3 * s, 48 * s * h, 6 * s, h > 0.5f ? green : h > 0.25f ? amber : red);
            }
            if (hp <= 0) {
                const char* m = "GEAR DESTROYED";
                hud.text(W * 0.5f - hud.textWidth(m, 2 * s) * 0.5f, H * 0.4f, m, red, 2 * s);
            }
        }
        // objectives (top left): active amber, complete green, failed red
        {
            float s2 = std::max(1.f, float(H) / 720.f), ox = 16 * s2, oy = 16 * s2;
            int shown = 0;
            for (auto& o : scripts.objectives)
                if (o.status && !o.text.empty()) shown++;
            if (shown && !cine) {
                hud.rect(ox - 6 * s2, oy - 6 * s2, 440 * s2, float(12 + 20 * shown) * s2, dim);
                for (auto& o : scripts.objectives) {
                    if (!o.status || o.text.empty()) continue;
                    uint32_t c = o.status == 2 ? green : o.status == 1 ? amber : red;
                    std::string t = std::string(o.status == 2 ? "[X] " : o.status == 1 ? "[ ] " : "[-] ") + o.text;
                    if (t.size() > 52) t = t.substr(0, 51) + "~";
                    hud.text(ox, oy, t, c, 0.8f * s2);
                    oy += 20 * s2;
                }
            }
            if (!scripts.screenText.empty() && scripts.time() < scripts.screenTextUntil) {
                const std::string& t = scripts.screenText;
                float sc = (hud.textWidth(t, 1.2f * s2) > W - 40 ? 0.9f : 1.2f) * s2;
                hud.text(W * 0.5f - hud.textWidth(t, sc) * 0.5f, H * 0.25f, t, amber, sc);
            }
        }
        if (!combat.log.empty()) {  // part-destroyed warnings
            damageMsg = combat.log.back(), damageMsgUntil = missionClock + 3.f;
            combat.log.clear();
        }
        if (!damageMsg.empty() && missionClock < damageMsgUntil) {
            float sc = 1.2f * s;
            hud.text(W * 0.5f - hud.textWidth(damageMsg, sc) * 0.5f, H * 0.62f, damageMsg, red, sc);
        }
        if (setup.net) {  // frags, top right under the radar
            std::vector<NetPlayer> ps = setup.net->players();
            std::sort(ps.begin(), ps.end(), [](const NetPlayer& a, const NetPlayer& b) { return a.kills > b.kills; });
            float y = H * 0.36f, sc = 0.9f * s;
            hud.text(W - 220 * s, y, "FRAGS", amber, sc);
            for (auto& p : ps) {
                y += 16 * s;
                std::string line = p.name + "  " + std::to_string(p.kills) + "/" + std::to_string(p.deaths);
                hud.text(W - 220 * s, y, line, p.id == setup.net->myId() ? amber : red, sc);
            }
        }
        if (!banner.empty() && scripts.time() < bannerUntil) {
            float sc = 1.5f * s;
            hud.text(W * 0.5f - hud.textWidth(banner, sc) * 0.5f, H * 0.18f, banner, amber, sc);
        }
        if (world.endCode >= 0) {
            const char* m = world.endCode == 1 ? "MISSION SUCCESSFUL" : world.endCode == 2 ? "MISSION OVER" : "MISSION FAILED";
            float sc = 2.f * s;
            hud.text(W * 0.5f - hud.textWidth(m, sc) * 0.5f, H * 0.32f, m, world.endCode == 0 ? red : green, sc);
        }
        if (float fa = scripts.fadeAlpha(); fa > 0.f)  // BeginFade
            hud.rect(0, 0, float(W), float(H), uint32_t(std::min(255.f, fa * 255.f)));
        if (missionClock < 8.f) {
            std::string t = !setup.title.empty() ? setup.title : scene.mission().title;
            std::string tl = lower(t);
            if (tl == "none" || tl == "temp title") t.clear();  // placeholder titles in some mission files
            if (!t.empty()) hud.text(W * 0.5f - hud.textWidth(t, 1.5f * s) * 0.5f, 40 * s, t, amber, 1.5f * s);
        }
        if (paused) {
            hud.rect(0, 0, float(W), float(H), 0x00000098u);
            const char* t = "PAUSED";
            hud.text(W * 0.5f - hud.textWidth(t, 2 * s) * 0.5f, H * 0.3f, t, amber, 2 * s);
            for (int i = 0; i < 3; i++) {
                float x, y, w, h;
                pauseItemRect(i, x, y, w, h);
                bool hov = pauseHover == i;
                hud.rect(x, y, w, h, hov ? 0x1c4a90e0u : 0x0c2448d0u);
                hud.frame(x, y, w, h, hov ? 0x8cc8ffffu : 0x3a6eb0ffu, s);
                hud.text(x + w * 0.5f - hud.textWidth(kPauseItems[i], 1.1f * s) * 0.5f, y + h * 0.5f - 8 * s, kPauseItems[i],
                         hov ? 0xffffffffu : 0x8cc8ffffu, 1.1f * s);
            }
            if (pauseMouseX >= 0) hud.rect(pauseMouseX - 2 * s, pauseMouseY - 2 * s, 4 * s, 4 * s, 0xffffffffu);
        }
        r.setOverlay(hud.vertices());
    };
    // ---------------------------------------------------------------- network play
    NetGame* net = setup.net;
    struct Remote {
        int id = -1, obj = -1;
        Player body;
        NetState st;
        bool have = false;
    };
    std::vector<std::unique_ptr<Remote>> remotes;
    std::vector<Vec3> spawnPoints;
    float netSendT = 0, respawnT = 0, matchT = 0;
    uint32_t netSeq = 0;
    bool deathSent = false, botClose = false;
    auto spawnPoint = [&](int id) {
        if (spawnPoints.empty()) return player.position();
        std::mt19937 g{uint32_t(id * 7919) ^ net->seed ^ uint32_t(SDL_GetTicks())};
        return spawnPoints[size_t(std::uniform_int_distribution<size_t>(0, spawnPoints.size() - 1)(g))];
    };
    auto netObjFor = [&](int netId) -> int {
        if (net && netId == net->myId()) return player.object();
        for (auto& r : remotes)
            if (r->id == netId) return r->obj;
        return -1;
    };
    auto netIdFor = [&](int obj) -> int {
        if (obj == player.object()) return net ? net->myId() : -1;
        for (auto& r : remotes)
            if (r->obj == obj) return r->id;
        return -1;
    };
    if (net && player.active()) {
        // spawn ring round the arena's start: every player gets a slot by id; respawns pick any of them
        Vec3 c = player.position();
        auto& ps = net->players();
        int n = std::max<int>(8, int(ps.size()));
        for (int k = 0; k < n; k++) {
            float a = 2.f * kPi * float(k) / float(n);
            Vec3 p = c + Vec3(std::sin(a), 0, std::cos(a)) * 120.f;
            p.x = std::clamp(p.x, 80.f, 4016.f), p.z = std::clamp(p.z, 80.f, 4016.f);
            p.y = scene.groundHeight(p.x, p.z);
            spawnPoints.push_back(p);
        }
        auto slotOf = [&](int id) {
            for (size_t i = 0; i < ps.size(); i++)
                if (ps[i].id == id) return spawnPoints[i % spawnPoints.size()];
            return c;
        };
        player.teleport(slotOf(net->myId()));
        { Vec3 d = c - player.position(); player.setYaw(std::atan2(-d.x, -d.z)); }
        Mission& m = scene.missionRW();
        for (auto& np : ps) {
            if (np.id == net->myId()) continue;
            auto r = std::make_unique<Remote>();
            r->id = np.id;
            MissionObject o;
            o.type = 0x48;
            o.team = userTeam + 1 + np.id % 8;  // every other player is a hostile
            o.name = np.name.empty() ? "player" + std::to_string(np.id) : np.name;
            Bytes g;
            if (!masterGear(fs, np.gear.chassis >= 0 ? np.gear.chassis : 0, g)) continue;
            o.defIndex = m.addDef(g.data(), g.size());
            Vec3 p = slotOf(np.id);
            o.pos[0] = p.x, o.pos[1] = p.y, o.pos[2] = p.z;
            r->obj = scene.addObject(o);
            combat.grow(m.objects.size());
            world.unitNames.push_back(o.name), world.unitTeams.push_back(o.team);
            if (!r->body.initObject(fs, scene, r->obj)) continue;
            combat.addUnit(r->obj, r->body.def());
            if (!np.gear.weapons.empty()) combat.setLoadout(r->obj, np.gear.weapons);
            if (np.gear.armor > 0) combat.setStats(r->obj, np.gear.armor, np.gear.fireControl);
            combat.setRemote(r->obj, true);
            if (audio) r->body.audio = audio;
            r->body.particles = &particles;
            remotes.push_back(std::move(r));
        }
        SDL_Log("net: %zu remote players, I am %d", remotes.size(), net->myId());
    }
    auto netStep = [&](float dt, bool fired, const Vec3& mz, const Vec3& aim) {
        if (!net) return;
        if (!net->poll() && !net->isHost()) world.missionOver(0, 1.f), banner = "CONNECTION LOST", bannerUntil = scripts.time() + 5.f;
        matchT += dt;
        if (fired) net->sendFire(mz, aim);
        if (fired && getenv("HG2_DEBUG")) SDL_Log("net: I fired");
        // my state, 20 times a second
        if ((netSendT -= dt) <= 0) {
            netSendT = 0.05f;
            NetState s;
            s.seq = ++netSeq, s.pos = player.position(), s.yaw = player.yaw(), s.twist = player.twist;
            s.throttle = player.speed(), s.posture = uint8_t(player.postureCode()), s.skates = player.skating();
            s.airborne = player.airborne(), s.alive = combat.alive(player.object());
            s.hp = combat.health(player.object());
            net->sendState(s);
        }
        for (auto& st : net->states)
            for (auto& r : remotes)
                if (r->id == st.id && (!r->have || st.seq > r->st.seq)) {
                    if (r->have && !r->st.alive && st.alive) {  // respawned over there
                        r->body.revive();
                        scene.clearDamage(r->obj);
                        combat.revive(r->obj, r->body.def());
                        r->body.teleport(st.pos);
                    }
                    r->st = st, r->have = true;
                    combat.setRemoteHealth(r->obj, st.alive != 0, st.hp);
                }
        net->states.clear();
        for (auto& f : net->fires) {
            int o = netObjFor(f.id);
            bool ok = o >= 0 && o != player.object() && combat.fireAs(o, f.muzzle, f.aim);
            if (getenv("HG2_DEBUG")) SDL_Log("net: fire from %d (obj %d) %s", f.id, o, ok ? "replayed" : "dropped");
        }
        net->fires.clear();
        for (auto& d : net->deaths) {
            SDL_Log("net: death of %d by %d", d.victim, d.killer);
            const NetPlayer* v = net->player(d.victim);
            const NetPlayer* k = net->player(d.killer);
            std::string vn = v ? v->name : "?", kn = k ? k->name : "?";
            banner = d.killer == d.victim || d.killer < 0 ? vn + " DIED" : kn + " KILLED " + vn;
            bannerUntil = scripts.time() + 3.f;
            int vo = netObjFor(d.victim);
            if (vo >= 0 && vo != player.object()) combat.setRemoteHealth(vo, false, 0);
        }
        net->deaths.clear();
        for (int id : net->left)
            for (auto& r : remotes)
                if (r->id == id && r->obj >= 0) scene.hideObject(r->obj), combat.setRemoteHealth(r->obj, false, 0), r->id = -1;
        net->left.clear();
        // drive the other players' gears: their controls animate the body, their positions place it
        for (auto& r : remotes) {
            if (!r->have || r->id < 0) continue;
            if (!r->st.alive) {
                if (!r->body.dead()) r->body.die();
                r->body.update(dt, PlayerInput{});
                continue;
            }
            Vec3 cur = r->body.position();
            Vec3 want = r->st.pos;
            PlayerInput in;
            float sp = r->st.throttle;
            float walk = r->body.def().maxWalkSpeed;
            in.throttle = std::fabs(sp) > 0.3f ? (sp > 0 ? 1.f : -1.f) : 0.f;
            in.run = std::fabs(sp) > walk * 1.1f;
            r->body.setPosture(r->st.posture == 3 ? 1 : r->st.posture);
            r->body.setSkates(r->st.skates != 0);
            r->body.twist = r->st.twist;
            r->body.update(dt, in);
            Vec3 d = want - cur;
            r->body.teleport(length(d) > 8.f ? want : cur + d * std::min(1.f, dt * 10.f));
            float dy = r->st.yaw - r->body.yaw();
            while (dy > kPi) dy -= 2 * kPi;
            while (dy < -kPi) dy += 2 * kPi;
            r->body.setYaw(r->body.yaw() + dy * std::min(1.f, dt * 12.f));
        }
        // my death and respawn
        bool alive = combat.alive(player.object());
        if (!alive && !deathSent) {
            deathSent = true;
            int k = netIdFor(combat.lastAttacker(player.object()));
            net->sendDeath(k);
            respawnT = 5.f;
        }
        if (!alive && deathSent && (respawnT -= dt) <= 0) {
            deathSent = false;
            player.revive();
            if (deathCam) deathCam = false, camPitch = savedPitch, camZoom = savedZoom, external = app.settings.externalView;
            scene.clearDamage(player.object());
            combat.revive(player.object(), player.def());
            player.teleport(spawnPoint(net->myId()));
            banner = "RESPAWNED", bannerUntil = scripts.time() + 2.f;
        }
        // the match ends at the frag limit or the time limit (the host decides)
        if (net->isHost() && !net->endReceived) {
            bool over = net->timeLimit > 0 && matchT > float(net->timeLimit);
            for (auto& p : net->players()) over = over || (net->fragLimit > 0 && p.kills >= net->fragLimit);
            if (over) SDL_Log("net: match over (%.0f s)", matchT), net->endMatch();
        }
        if (getenv("HG2_DEBUG") && int(matchT) != int(matchT - dt))
            for (auto& r : remotes) SDL_Log("net: remote %d at %.0f,%.0f alive %d hp %.2f; me %.0f,%.0f hp %.2f", r->id, r->st.pos.x, r->st.pos.z, r->st.alive, r->st.hp,
                                             player.position().x, player.position().z, combat.health(player.object()));
        if (net->endReceived && world.endCode < 0) {
            int best = -1, mine = 0;
            for (auto& p : net->players()) {
                best = std::max(best, p.kills);
                if (p.id == net->myId()) mine = p.kills;
            }
            world.missionOver(mine >= best ? 1 : 0, 4.f);
            banner = mine >= best ? "YOU WIN THE MATCH" : "MATCH OVER", bannerUntil = scripts.time() + 5.f;
        }
    };
    auto finish = [&](MissionResult::Outcome o) {
        res.outcome = o;
        res.time = scripts.time();
        res.kills = combat.killsBy[player.object()];
        res.shots = combat.shotsBy[player.object()];
        res.hits = combat.hitsBy[player.object()];
        return res;
    };
    if (offscreen) {
        if (getenv("HG2_GOD")) combat.setInvincible(player.object(), true);  // test runs
        for (int f = 0; f < T.driveFrames; f++) {
            PlayerInput pin;
            pin.throttle = T.startX >= 0 ? 0.f : 1.f;
            pin.run = f > T.driveFrames / 2;
            pin.turn = T.startX >= 0 ? 0.f : 0.3f;
            const bool straight = getenv("HG2_STRAIGHT") != nullptr;  // test: straight line, walk / run / coast / run
            if (straight) {
                int q = f * 4 / std::max(1, T.driveFrames);
                pin.turn = 0, pin.run = q == 1 || q == 3;
                pin.throttle = q == 2 ? 0.f : pin.run ? 1.f : 0.2f;  // walk: throttle 0.2 holds ~0.45 vmax
            }
            if (T.startX >= 0 && f % 3 == 0 && !getenv("HG2_DRIVETO") && !straight) {  // test mode: hold position and fire forward
                Vec3 mz, aim;
                aimPoint(mz, aim);
                combat.fire(mz, aim);
            }
            float np[3];
            const char* driveTo = getenv("HG2_DRIVETO");  // test: walk to x,z (collision checks) instead of the nav point
            bool toPoint = driveTo && sscanf(driveTo, "%f,%f", &np[0], &np[2]) == 2;
            if (toPoint || (T.navDrive && scripts.checkpointPosition(scripts.selectedCheckpoint, np))) {
                Vec3 d = Vec3(np[0], 0, np[2]) - Vec3(player.position().x, 0, player.position().z);
                float want = std::atan2(-d.x, -d.z), e = want - player.yaw();
                while (e > kPi) e -= 2 * kPi;
                while (e < -kPi) e += 2 * kPi;
                pin.turn = std::max(-1.f, std::min(1.f, e * 2.f));
                pin.throttle = std::fabs(e) < 0.6f && length(d) > 4.f ? 1.f : 0.f;
                pin.run = length(d) > 40.f && !toPoint;
            } else if (T.navDrive) {
                pin = PlayerInput{};
            }
            for (auto& [fr, act] : T.inputScript) {
                if (fr != f) continue;
                if (act == "down") pin.down = true;
                else if (act == "up") pin.up = true;
                else if (act == "jump") pin.jump = true;
                else if (act == "jets") pin.jumpJets = true;
                else if (act == "skates") pin.toggleSkates = true;
                else if (act == "view") external = !external;
                else if (act == "flinch") player.hitReaction(3);
                else if (act.rfind("order", 0) == 0) pickTarget(), issueOrder(atoi(act.c_str() + 5));
            }
            if (const char* st = getenv("HG2_STRAFE")) pin.strafe = float(atof(st));  // test: hold a sidestep key
            if (getenv("HG2_IDLE")) pin = PlayerInput{};  // test: the user leaves the controls alone (as a windowed autoend run)
            if (const char* th = getenv("HG2_THRUST")) {  // test: the space thruster command, and the body's track
                pin.thrust = float(atof(th));
                if (f % 15 == 0)
                    printf("frame %d: pos %.1f %.1f %.1f speed %.2f\n", f, player.position().x, player.position().y,
                           player.position().z, player.speed());
            }
            if (const char* kt = getenv("HG2_KILLMATE"))  // test: destroy squad mate 0 outright (0x100000) at frame n
                if (f == atoi(kt) && !mates.empty()) {
                    Vec3 mp;
                    scene.objectPosition(mates[0], mp);
                    combat.testHit(mates[0], 1e7f, mp);
                    printf("killmate: ejected %d, weapons on the ground %zu, at %.1f %.1f %.1f\n", int(combat.ejected(mates[0])), combat.droppedWeapons(), mp.x, mp.y, mp.z);
                }
            if (const char* ct = getenv("HG2_CRIPPLE"))  // test: Movement critical 0x40000 on the user's gear at frame n
                if (f == atoi(ct)) combat.testCritical(player.object(), 0x40000);
            if (!world.control) pin = PlayerInput{};
            userAutopilot(pin, false);
            if (getenv("HG2_BOLT") && f == std::max(0, T.driveFrames - 3)) {  // test: a lightning bolt 60 m ahead
                Vec3 eye = frameParams().eye, d = normalize(player.position() - eye);
                particles.testStrike(Vec3(eye.x + d.x * 60.f, eye.y, eye.z + d.z * 60.f));
            }
            if (const char* fxn = getenv("HG2_FX"))  // test: an effect 25 m ahead, a few frames before the shot
                if (f == std::max(0, T.driveFrames - atoi(getenv("HG2_FX_AGE") ? getenv("HG2_FX_AGE") : "10"))) {
                    Vec3 eye = frameParams().eye, at = player.position() + (eye - player.position()) * 0.5f;  // between camera and gear
                    at.y = scene.groundHeight(at.x, at.z);
                    printf("effect %s: %s\n", fxn, particles.spawn(fxn, at, 1.5f) ? "ok" : "not found");
                }
            player.speedScale = combat.mobility(player.object()), player.maneuverNow = combat.maneuverNow(player.object());
            player.jumpJets = combat.jumpJets(player.object());
            if (int kd = combat.takeKnock(player.object())) player.knockDown(kd);
            if (combat.takeCripple(player.object())) player.cripple();
            if (player.active() && !combat.alive(player.object())) player.die();
            player.update(1.f / 30.f, pin);
            if (float v = player.takeImpact()) combat.hardLanding(player.object(), v);
            combat.setSpeed(player.object(), player.speed());
            for (int o : player.takeRams()) combat.ram(o, player.object());
            ai.setCombatHold(scripts.combatHold());
            updateStrikes(1.f / 30.f);
            ai.update(1.f / 30.f);
            updateFlyers(1.f / 30.f);
            combat.update(1.f / 30.f);
            particles.setWind(world.windV, world.gust);
            particles.setViewer(frameParams().eye);
            particles.update(1.f / 30.f);
            {
                FrameParams vf = frameParams();
                viewProj = vf.view * vf.proj, haveView = true;
            }
            fountainStep(1.f / 30.f);
            iaStep();
            drainCombatEvents();
            scripts.update(1.f / 30.f);
            if (f % 600 == 0 && getenv("HG2_AIDBG")) ai.dump();
            if (f % (driveTo ? 30 : 300) == 0 && (T.navDrive || driveTo || !T.inputScript.empty()))
                printf("t=%5.1f pos %.0f,%.0f y %.1f v %.1f ctl %d posture %d nav %d %s | %s\n", scripts.time(), player.position().x,
                       player.position().z, player.position().y, player.speed(), int(world.control) + 10 * int(player.forwardLocked()), player.postureCode(),
                       scripts.selectedCheckpoint,
                       scripts.selectedCheckpoint >= 0 ? scripts.checkpoints[size_t(scripts.selectedCheckpoint)].label.c_str() : "-",
                       scripts.screenText.c_str());
        }
        if (T.driveFrames == 0) player.update(0.f, PlayerInput{});
        if (getenv("HG2_PROF") && T.driveFrames > 0)
            printf("prof: collision %.3f ms a frame (%.0f queries a frame)\n", scene.profNs * 1e-6 / T.driveFrames,
                   double(scene.profCalls) / T.driveFrames);
        if (getenv("HG2_OBJY")) {
            double e1 = 0, e2 = 0;
            int n = 0;
            for (auto& o : scene.mission().objects) {
                float x = float(o.pos[0]), y = float(o.pos[1]), z = float(o.pos[2]);
                e1 += std::fabs(y - scene.groundHeight(x, z)), e2 += std::fabs(y - scene.groundHeight(z, x)), n++;
            }
            printf("objects %d: mean |y-ground| %.1f, transposed %.1f\n", n, e1 / n, e2 / n);
        }
        if (const char* pn = getenv("HG2_PATH"))
            if (auto* pa = scene.mission().path(pn))
                for (size_t i = 0; i < pa->points.size(); i += 4)
                    printf("p %zu %.0f,%.1f,%.0f ground %.1f\n", i, pa->points[i][0], pa->points[i][1], pa->points[i][2],
                           scene.groundHeight(pa->points[i][0], pa->points[i][2]));
        if (T.sideView && player.active()) {
            float yw = player.yaw();
            Vec3 side(-std::cos(yw), 0, std::sin(yw));
            cam.pos = player.position() + side * 14.f + Vec3(0, 4, 0);
            Vec3 d = normalize(player.position() + Vec3(0, 3, 0) - cam.pos);
            cam.yaw = std::atan2(d.x, d.z), cam.pitch = std::asin(d.y);
            chase = false;
        }
        if (const char* la = getenv("HG2_LOOKAT")) {  // test: a camera 12 m off a point, looking at it
            Vec3 at;
            if (sscanf(la, "%f,%f,%f", &at.x, &at.y, &at.z) == 3) {
                cam.pos = at + Vec3(9.f, 6.f, -9.f);
                Vec3 d = normalize(at + Vec3(0, 2, 0) - cam.pos);
                cam.yaw = std::atan2(d.x, d.z), cam.pitch = std::asin(d.y);
                chase = false;
            }
        }
        scene.setObjectDrawn(player.object(), external || !chase || scripts.cinematic.active);
        if (T.driveFrames) printf("after %d frames: %s, hostiles alive %d/%d, squad %zu, fountain units %d, end %d\n", T.driveFrames, combat.status().c_str(),
                                  ai.alive(), ai.count(), mates.size(), fountains.spawned(), world.endCode);
        if (const char* wn = getenv("HG2_WEAPON"); wn && T.fireFrames && !combat.testWeapon(wn)) fprintf(stderr, "no weapon %s\n", wn);
        for (int f = 0; f < T.fireFrames; f++) {
            Vec3 mz, aim;
            aimPoint(mz, aim);
            combat.fire(mz, aim);
            combat.update(1.f / 30.f);
            particles.update(1.f / 30.f);
        }
        buildHud(frameParams());
        particles.setViewer(frameParams().eye);
        particles.setWater(scene.mission().hasWater, scene.waterLevel());
        particles.submit(frameParams().view, frameParams().proj);
        if (r.beginFrame(frameParams())) {
            scene.render(frameParams());
            combat.draw();
            r.drawDynamic();
            r.endFrame();
        }
        std::vector<uint8_t> px;
        int w, h;
        if (!T.screenshot.empty()) {
            if (!r.readback(px, w, h) || !writePngFile(T.screenshot, px, w, h)) {
                fprintf(stderr, "screenshot failed\n");
                res.error = "screenshot failed";
                return res;
            }
            printf("wrote %s (%dx%d)\n", T.screenshot.c_str(), w, h);
        }
        return finish(world.endCode == 1 ? MissionResult::Success : world.endCode == 2 ? MissionResult::Alternate
                                                                    : world.endCode == 0 ? MissionResult::Failure : MissionResult::Aborted);
    }
    SDL_Window* win = app.win;
    SDL_SetWindowRelativeMouseMode(win, true);
    PlayerInput edge;  // one-shot actions gathered from key presses
    // throttle setting (+0x1b18, FUN_10063a10): 1/3, 2/3, full, increase / decrease by 0.05, Full Stop; Reverse
    // Direction flips the mode (+0x1b1c, forward after Full Stop and at the start)
    float throttleSet = 0.f;
    bool throttleForward = true;
    Uint64 last = SDL_GetTicksNS();
    float deadTime = 0;
    const Bindings& keys = app.settings.bindings;
    // the free camera is a port viewing tool with no counterpart in the original (its views are cockpit, external,
    // the scripted cameras and the tactical map): HG2_FREECAM=1 starts the mission in it and lets F1 toggle it
    const bool freecam = getenv("HG2_FREECAM") && atoi(getenv("HG2_FREECAM")) != 0;
    auto enterFreecam = [&]() {  // the free camera starts where the view was
        Vec3 eye, at;
        viewCamera(eye, at);
        cam.pos = eye;
        Vec3 d = normalize(at - eye);
        cam.yaw = std::atan2(d.x, d.z);
        cam.pitch = std::asin(d.y);
    };
    if (freecam && chase) enterFreecam(), chase = false;
    auto setPaused = [&](bool on) {
        paused = on;
        SDL_SetWindowRelativeMouseMode(win, !on);
        pauseHover = -1, pauseMouseX = -1;
        if (audio) {
            if (on) audio->stopAll();
        }
    };
    for (;;) {
        SDL_Event ev;
        while (SDL_PollEvent(&ev)) {
            if (ev.type == SDL_EVENT_QUIT) {
                app.quit = true;
                return finish(MissionResult::Quit);
            }
            if (paused) {
                if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE) setPaused(false);
                else if (ev.type == SDL_EVENT_KEY_DOWN && (ev.key.key == SDLK_UP || ev.key.key == SDLK_DOWN))
                    pauseHover = (std::max(0, pauseHover) + (ev.key.key == SDLK_UP ? 2 : 1)) % 3;
                else if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                    pauseMouseX = ev.motion.x * SDL_GetWindowPixelDensity(win), pauseMouseY = ev.motion.y * SDL_GetWindowPixelDensity(win);
                    pauseHover = -1;
                    for (int i = 0; i < 3; i++) {
                        float x, y, w, h;
                        pauseItemRect(i, x, y, w, h);
                        if (pauseMouseX >= x && pauseMouseX < x + w && pauseMouseY >= y && pauseMouseY < y + h) pauseHover = i;
                    }
                } else if ((ev.type == SDL_EVENT_MOUSE_BUTTON_UP && pauseHover >= 0) ||
                           (ev.type == SDL_EVENT_KEY_DOWN && (ev.key.key == SDLK_RETURN || ev.key.key == SDLK_KP_ENTER) && pauseHover >= 0)) {
                    if (pauseHover == 0) setPaused(false);
                    else if (pauseHover == 1) {
                        SDL_SetWindowRelativeMouseMode(win, false);
                        return finish(MissionResult::Aborted);
                    } else {
                        app.quit = true;
                        return finish(MissionResult::Quit);
                    }
                }
                continue;
            }
            // the commands by the user's bindings (the controls screen; the shell's Defaults.dft until changed). The
            // port adds only keys the original left unbound: Space (jump, or skip a camera flight), 1-8 (select a
            // weapon), M (music), V (Sniper Systems zoom) and the mouse wheel (zoom); a binding on them wins.
            auto extra = [&](SDL_Scancode sc) { return ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat && ev.key.scancode == sc && !keys.usesKey(sc); };
            auto cmd = [&](Cmd c) { return keys.pressed(c, ev); };
            if (cmd(Cmd::Abort)) setPaused(true);
            else if (ev.type == SDL_EVENT_WINDOW_FOCUS_LOST && !net) setPaused(true);
            else if (freecam && ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat && ev.key.scancode == SDL_SCANCODE_F1 && player.active()) {
                chase = !chase;
                if (!chase) enterFreecam();
            } else if (ev.type == SDL_EVENT_MOUSE_WHEEL) {
                // wheel: Zoom In / Zoom Out steps (0.25 radius, 2 .. 8)
                if (!deathCam) camZoom = std::max(2.f, std::min(8.f, camZoom - ev.wheel.y * 0.25f));
            } else if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                if (chase) {
                    // Target ReticleX / Y on the mouse axes (type 3, FUN_10136b40): -mickeys x 0.005 x the binding's
                    // sensitivity a poll, added to the reticle and held to -1 .. 1 (FUN_10117fb0); Invert Mouse Y flips Y
                    reticleX = std::clamp(reticleX + ev.motion.xrel * 0.005f * keys.axisScale(Cmd::ReticleX, 0), -1.f, 1.f);
                    float inv = app.settings.invertMouse ? -1.f : 1.f;
                    reticleY = std::clamp(reticleY - ev.motion.yrel * 0.005f * keys.axisScale(Cmd::ReticleY, 1) * inv, -1.f, 1.f);
                } else {
                    const float sens = 0.003f;
                    cam.yaw += ev.motion.xrel * sens;
                    cam.pitch = std::max(-1.5f, std::min(1.5f, cam.pitch - ev.motion.yrel * sens));
                }
            } else if ((ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) || ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN) {
                if (extra(SDL_SCANCODE_SPACE) && (scripts.cinematic.active || scripts.cinematicMode)) scripts.skipCinematic();
                else if (cmd(Cmd::Jump) && scripts.cinematic.active) scripts.skipCinematic();
                else if (cmd(Cmd::Jump) || extra(SDL_SCANCODE_SPACE)) edge.jump = chase;
                for (int i = 0; i < 8; i++)
                    if (extra(SDL_Scancode(SDL_SCANCODE_1 + i))) combat.selectWeapon(i);
                if (cmd(Cmd::WeaponCycle)) combat.nextWeapon();
                if (cmd(Cmd::Crouch)) edge.down = true;
                if (cmd(Cmd::StandUp)) edge.up = true;
                if (cmd(Cmd::JumpJets)) edge.jumpJets = chase;
                if (cmd(Cmd::Wheels)) edge.toggleSkates = true;
                // throttle setting (FUN_10063a10): 1/3 (0x3eaaa64c), 2/3 (0x3f2aa64c), full, Full Stop (forward mode),
                // Reverse Direction
                if (cmd(Cmd::Throttle13)) throttleSet = 0.333f;
                if (cmd(Cmd::Throttle23)) throttleSet = 0.6666f;
                if (cmd(Cmd::FullThrottle)) throttleSet = 1.f;
                if (cmd(Cmd::FullStop)) throttleSet = 0.f, throttleForward = true;
                if (cmd(Cmd::ReverseDirection)) throttleForward = !throttleForward;
                if (cmd(Cmd::ExternalTracking)) {  // FUN_10093490 -> FUN_10092ff0(0/1); also leaves a scripted camera
                    external = !external, app.settings.externalView = external;
                    if (scripts.cinematic.active || scripts.cinematicMode) scripts.releaseCamera();
                }
                if (cmd(Cmd::SelectNavPoint)) scripts.stepCheckpoint(1);
                if (extra(SDL_SCANCODE_M) && audio) audio->musicVolume = audio->musicVolume > 0 ? 0.f : app.settings.music;
                if (extra(SDL_SCANCODE_V) && combat.sniper(player.object())) sniperZoom = !sniperZoom;  // Sniper Systems zoom
                // squad commands (FUN_100d1bc0)
                if (cmd(Cmd::SquadAttackMyTarget)) issueOrder(0);
                if (cmd(Cmd::SquadDefendMyTarget)) issueOrder(1);
                if (cmd(Cmd::SquadHoldGround)) issueOrder(2);
                if (cmd(Cmd::SquadRegroup)) issueOrder(3);
                if (cmd(Cmd::SquadFireAtWill)) issueOrder(4);
                if (cmd(Cmd::SquadGotoCheckpoint)) issueOrder(5);
                if (cmd(Cmd::EcmEnable) && chase) combat.toggleEcm(player.object());
                if (cmd(Cmd::ActiveRadarConfig) && chase) combat.setActiveSensors(player.object(), !combat.activeSensors(player.object()));
                // targeting (FUN_10114210)
                if (cmd(Cmd::TargetAtReticle)) pickTarget();
                if (cmd(Cmd::TargetCycle)) cycleTarget(1, false);
                if (cmd(Cmd::PreviousTarget)) cycleTarget(-1, false);
                if (cmd(Cmd::NearestEnemy)) cycleTarget(0, false);
                if (cmd(Cmd::TargetFriendly)) cycleTarget(0, true);
            }
        }
        Uint64 now = SDL_GetTicksNS();
        // the original's frame clock (FUN_1002a190): no frame shorter than 1/60 s (Combat::kFrameHz)
        const Uint64 minFrame = Uint64(1e9 / double(Combat::kFrameHz));
        if (now - last < minFrame) SDL_DelayPrecise(minFrame - (now - last)), now = SDL_GetTicksNS();
        float dt = std::min(float(now - last) * 1e-9f, 0.1f);
        if (getenv("HG2_PROF")) {  // frame time and the collision queries' share, every 5 s
            static Uint64 since = now, frames = 0;
            frames++;
            if (now - since > 5000000000ull) {
                SDL_Log("prof: %.1f ms a frame, collision %.2f ms a frame (%.0f queries)", (now - since) * 1e-6 / frames,
                        scene.profNs * 1e-6 / frames, double(scene.profCalls) / frames);
                since = now, frames = 0, scene.profNs = 0, scene.profCalls = 0;
            }
        }
        last = now;
        if (paused) {
            if (net) net->poll();  // keep the connection alive; the world goes on without us
            FrameParams fp = frameParams();
            buildHud(fp);
            particles.setWater(scene.mission().hasWater, scene.waterLevel());
            particles.submit(fp.view, fp.proj);
            if (r.beginFrame(fp)) {
                scene.render(fp);
                combat.draw();
                r.drawDynamic();
                r.endFrame();
            }
            continue;
        }
        scripts.update(dt);
        scene.setTime(scripts.time());
        const bool* k = SDL_GetKeyboardState(nullptr);
        PlayerInput pin;
        const SDL_MouseButtonFlags mbuttons = SDL_GetMouseState(nullptr, nullptr);
        auto held = [&](Cmd c) { return keys.held(c, k, mbuttons); };
        if (chase) {
            pin.throttle = (held(Cmd::Forward) ? 1.f : 0.f) - (held(Cmd::Backward) ? 1.f : 0.f);
            pin.turn = (held(Cmd::SteerRight) ? 1.f : 0.f) - (held(Cmd::SteerLeft) ? 1.f : 0.f);
            pin.strafe = (held(Cmd::SideStepRight) ? 1.f : 0.f) - (held(Cmd::SideStepLeft) ? 1.f : 0.f);
            pin.run = true;
            // Increase / Decrease Throttle (held): +-0.05 a poll, up to 1, down to 0.05 (FUN_10063a10). In space they
            // leave the setting alone and command the thrusters instead (+0x1040 = +-1), and the setting is added
            // (forward mode) or taken off (reverse)
            const bool inc = held(Cmd::IncreaseThrottle), dec = held(Cmd::DecreaseThrottle);
            if (space) {
                pin.thrust = inc ? 1.f : dec ? -1.f : 0.f;
                pin.thrust += throttleForward ? throttleSet : -throttleSet;
            } else if (inc) {
                if (throttleSet < 1.f) throttleSet = std::min(1.f, throttleSet + 0.05f);
            } else if (dec) {
                if (throttleSet > 0.05f) throttleSet -= 0.05f;
            }
            // Forward / Backward is an axis (-1 .. 1); steering with no axis drives at 0.05, reverse is held to
            // -0.5; then the setting: in forward mode the axis is at least the setting, in reverse the setting backs
            // the gear at half of it
            if (std::fabs(pin.turn) > 0.05f && std::fabs(pin.throttle) < 0.05f) pin.throttle = 0.05f;
            if (pin.throttle < -0.5f) pin.throttle = -0.5f;
            if (!throttleForward) pin.throttle -= throttleSet * 0.5f;
            else if (pin.throttle >= 0 && pin.throttle < throttleSet) pin.throttle = throttleSet;
            pin.down = edge.down, pin.up = edge.up, pin.jump = edge.jump, pin.jumpJets = edge.jumpJets;
            pin.toggleSkates = edge.toggleSkates;
            if (held(Cmd::CenterTorso)) reticleX = reticleY = 0.f;  // FUN_10117fb0: Center Torso centres the reticle
            // Camera Left / Right / Up / Down and Zoom In / Out, FUN_1007c840 once a frame: 2.5 deg and 0.25 radius a
            // poll, one poll a frame at the original's frame rate (Combat::kFrameHz); the pitch holds to -30 .. 80 deg, the zoom to 2 .. 8, the orbit
            // is free (no drift back)
            const float polls = Combat::kFrameHz * dt;
            camOrbit += ((held(Cmd::CameraRight) ? 1.f : 0.f) - (held(Cmd::CameraLeft) ? 1.f : 0.f)) * kCamStep * polls;
            if (held(Cmd::CameraUp) && !held(Cmd::CameraDown)) camPitch = std::min(kPitchMax, camPitch + kCamStep * polls);
            if (held(Cmd::CameraDown) && !held(Cmd::CameraUp)) camPitch = std::max(kPitchMin, camPitch - kCamStep * polls);
            if (!deathCam && held(Cmd::ZoomIn)) camZoom = std::max(2.f, camZoom - 0.25f * polls);
            if (!deathCam && held(Cmd::ZoomOut)) camZoom = std::min(8.f, camZoom + 0.25f * polls);
        }
        bool dead = player.active() && !combat.alive(player.object());
        if (dead) player.die();
        if (dead && !deathCam && !scripts.cinematic.active) {
            // FUN_10094700: the user's unit no longer alive switches to the death camera (FUN_10092ff0(8)): external,
            // pitch and zoom saved, 45 deg at 20 radii
            deathCam = true, external = true;
            savedPitch = camPitch, savedZoom = camZoom;
            camPitch = 0.78539805f, camZoom = 20.f;
        }
        if (dead || !world.control) pin = PlayerInput{};
        userAutopilot(pin, dead);
        player.twist = 0.f;  // the user's torso has no twist input (the sidestep holds its own angle in player.cpp)
        edge = PlayerInput{};
        player.speedScale = combat.mobility(player.object()), player.maneuverNow = combat.maneuverNow(player.object());
        player.jumpJets = combat.jumpJets(player.object());
        if (int kd = combat.takeKnock(player.object())) player.knockDown(kd);
        if (combat.takeCripple(player.object())) player.cripple();
        combat.setPosture(player.object(), player.postureCode());
        // the user's visual sensor looks along its pov frame (Combat::sensorAxis)
        if (botClose && !dead) pin.throttle = 1;
        player.update(dt, pin);
        if (float v = player.takeImpact()) combat.hardLanding(player.object(), v);
            combat.setSpeed(player.object(), player.speed());
            for (int o : player.takeRams()) combat.ram(o, player.object());
        scene.setObjectDrawn(player.object(), external || !chase || scripts.cinematic.active);
        ai.setCombatHold(scripts.combatHold());
        updateStrikes(dt);
        ai.update(dt);
        updateFlyers(dt);
        if (audio) {
            if (chase) {
                Vec3 eye, at;
                viewCamera(eye, at);
                audio->setListener(eye, std::atan2(-(at - eye).x, -(at - eye).z));
            } else {
                audio->setListener(cam.pos, std::atan2(-cam.forward().x, -cam.forward().z));
            }
            audio->update();
            updateAmbient();  // EAR moves its events every frame (FUN_1012e708)
        }
        validateTarget();
        bool fired = false;
        Vec3 fmz, faim;
        if (chase && !dead && world.control && held(Cmd::WeaponFire)) {
            aimPoint(fmz, faim);
            combat.setAimTarget(player.object(), userTarget);
            fired = combat.fire(fmz, faim);
        }
        if (net && getenv("HG2_NETBOT") && !dead)  // test: shoot at the first other player in range
            for (auto& r : remotes) {
                Vec3 tp;
                if (!r->have || !r->st.alive || !scene.objectPosition(r->obj, tp)) continue;
                Vec3 d = tp - player.position();
                player.setYaw(std::atan2(-d.x, -d.z));
                botClose = length(d) > 35.f;
                if (length(d) < 150.f) {
                    if (!combat.muzzle(player.object(), fmz)) fmz = player.position() + Vec3(0, 5.f, 0);
                    faim = tp + Vec3(0, 3.5f, 0);
                    fired = combat.fire(fmz, faim) || fired;
                }
                break;
            }
        netStep(dt, fired, fmz, faim);
        combat.update(dt);
        particles.setWind(world.windV, world.gust);
        particles.setViewer(frameParams().eye);
        particles.update(dt);
        weatherSound();
        {
            FrameParams vf = frameParams();
            viewProj = vf.view * vf.proj, haveView = true;
        }
        fountainStep(dt);
        iaStep();
        drainCombatEvents();
        if (getenv("HG2_AIDBG") && int(scripts.time() / 10) != int((scripts.time() - dt) / 10)) ai.dump();
        if (const char* ws = getenv("HG2_WSIGNAL")) {  // test automation: "t:sig[:unit]" sends a world (or unit) signal once at t s
            static bool sent = false;
            float at = 0;
            int sig = -1;
            char unit[64] = {};
            int n = sscanf(ws, "%f:%d:%63s", &at, &sig, unit);
            if (!sent && n >= 2 && scripts.time() > at) {
                sent = true;
                if (n == 3) scripts.unitSignal(world.findUnit(unit), sig);
                else scripts.worldSignal(sig);
            }
        }
        if (const char* ae = getenv("HG2_AUTOEND"))  // test automation: succeed after n seconds
            if (scripts.time() > float(atof(ae)) && world.endCode < 0) world.missionOver(1, 1.f);
        // the user's gear destroyed with no script ending the mission: failure after a while
        deadTime = dead ? deadTime + dt : 0.f;
        if (dead && deadTime > 8.f && world.endCode < 0 && !net) world.missionOver(0, 0.f);
        if (world.endCode >= 0) {
            world.endDelay -= dt;
            if (world.endDelay <= 0) {
                SDL_SetWindowRelativeMouseMode(win, false);
                return finish(world.endCode == 1 ? MissionResult::Success : world.endCode == 2 ? MissionResult::Alternate : MissionResult::Failure);
            }
        }
        float speed = chase ? 0.f : (k[SDL_SCANCODE_LSHIFT] ? 200.f : 40.f) * dt;
        Vec3 f = cam.forward(), right = normalize(cross(Vec3(0, 1, 0), f));
        if (k[SDL_SCANCODE_W]) cam.pos += f * speed;
        if (k[SDL_SCANCODE_S]) cam.pos += f * -speed;
        if (k[SDL_SCANCODE_D]) cam.pos += right * speed;
        if (k[SDL_SCANCODE_A]) cam.pos += right * -speed;
        if (k[SDL_SCANCODE_SPACE]) cam.pos.y += speed;
        if (k[SDL_SCANCODE_LCTRL]) cam.pos.y -= speed;
        FrameParams fp = frameParams();
        missionClock += dt;
        buildHud(fp);
        particles.setWater(scene.mission().hasWater, scene.waterLevel());
            particles.submit(fp.view, fp.proj);
        if (r.beginFrame(fp)) {
            scene.render(frameParams());
            combat.draw();
            r.drawDynamic();
            r.endFrame();
        }
    }
}

}  // namespace hg2
