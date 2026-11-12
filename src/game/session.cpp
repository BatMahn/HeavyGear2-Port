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

// formation slots behind the user (x right, z behind)
Vec3 formationSlot(size_t i) {
    float row = float(i / 2 + 1);
    return Vec3((i % 2 ? 1.f : -1.f) * 11.f * row, 0, 13.f * row);
}
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
    std::vector<Vec3> mateSlots;
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
        float ly = float(base.rot[0]) * kDeg;
        Vec3 right(-std::cos(ly), 0, std::sin(ly)), back(std::sin(ly), 0, std::cos(ly));
        for (size_t i = 0; i < setup.squad.size(); i++) {
            MissionObject o;
            o.type = 0x48;
            o.team = base.team;
            o.name = "mate" + std::to_string(i + 1);
            for (int k = 0; k < 3; k++) o.rot[k] = base.rot[k];
            o.defIndex = base.defIndex;
            Bytes mg;
            if (setup.squad[i].gear.chassis >= 0 && masterGear(fs, setup.squad[i].gear.chassis, mg)) o.defIndex = m.addDef(mg.data(), mg.size());
            Vec3 slot = formationSlot(i), p = right * slot.x + back * slot.z;
            o.pos[0] = base.pos[0] + p.x, o.pos[1] = base.pos[1], o.pos[2] = base.pos[2] + p.z;
            m.objects.push_back(o);
            mates.push_back(int(m.objects.size()) - 1);
            mateSlots.push_back(slot);
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
        void unitGoto(int u, const float p[3]) override { if (ai) ai->gotoPoint(u, Vec3(p[0], p[1], p[2])); }
        bool unitFollowPath(int u, const std::string& n) override {
            bool ok = ai && ai->followPath(u, n);
            if (!ok && verbose && !n.empty() && n.size() < 40)
                SDL_Log("[script] FollowPath %s for unit %d (%s) failed", n.c_str(), u,
                        u >= 0 && size_t(u) < m->objects.size() ? m->objects[size_t(u)].name.c_str() : "-");
            return ok;
        }
        bool unitAssignPath(int u, const std::string& n) override { return ai && ai->assignPath(u, n); }
        bool verbose = false;
        void unitStop(int u) override { if (ai) ai->stop(u); }
        void unitSpeed(int u, float k) override { if (ai) ai->speed(u, k); }
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
        void unitCommand(int u, const std::string& c, float a, float b, float cc, float, const std::string& str) override {
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
            } else if (ai) {
                ai->command(u, c, a, b, cc, str);
            }
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
        int weatherType = 0, weatherPreset = 0;
        void weather(int type, int preset) override {
            if (type >= 0) weatherType = type;
            if (preset >= 0) weatherPreset = preset;
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
    float camPitch = 0.25f, camOrbit = 0;
    if (player.active() && T.startX >= 0) {
        player.teleport(Vec3(T.startX, 0, T.startZ));
        player.setYaw(T.startYaw * kDeg);
    }
    Combat combat;
    if (player.active()) combat.init(fs, scene, player.def(), player.object());
    if (player.active() && !setup.player.weapons.empty()) combat.setLoadout(player.object(), setup.player.weapons);
    if (player.active() && setup.player.armor > 0) combat.setStats(player.object(), setup.player.armor, setup.player.fireControl);
    if (player.active() && setup.player.hasSystems)
        combat.setSystems(player.object(), setup.player.sensors, setup.player.maneuver, setup.player.perks, setup.player.flaws);
    AiSystem ai;
    if (player.active()) ai.init(fs, scene, combat, player);
    ai.setDifficulty(setup.difficulty);
    combat.setSpace(setup.space);
    if (setup.space) player.gravityScale = 0.35f, ai.setGravity(0.35f);
    world.ai = &ai;
    for (size_t i = 0; i < mates.size(); i++) {
        ai.follow(mates[i], player.object(), mateSlots[i]);
        if (!setup.squad[i].gear.weapons.empty()) combat.setLoadout(mates[i], setup.squad[i].gear.weapons);
        if (setup.squad[i].gear.armor > 0) combat.setStats(mates[i], setup.squad[i].gear.armor, setup.squad[i].gear.fireControl);
        const GearChoice& g = setup.squad[i].gear;
        if (g.hasSystems) combat.setSystems(mates[i], g.sensors, g.maneuver, g.perks, g.flaws);
    }
    Audio* audio = app.audioOk && !offscreen ? &app.audio : nullptr;
    if (audio) {
        world.audio = audio;
        combat.audio = audio;
    }
    world.combat = &combat;
    world.player = player.object();
    world.pl = &player;
    if (const char* w = getenv("HG2_WEATHER")) sscanf(w, "%d,%d", &world.weatherType, &world.weatherPreset);  // test
    FountainSystem fountains;
    fountains.init(scene.mission());
    world.fountains = &fountains;
    // reinforcements from the mission's unit generators
    auto spawnUnit = [&](const FountainTemplate& t, const Fountain& f, int n) -> int {
        MissionObject o;
        o.type = t.type, o.defIndex = t.defIndex, o.team = f.team;
        float ang = float(n) * 2.1f, rr = n ? 12.f : 0.f;
        o.pos[0] = f.pos[0] + std::cos(ang) * rr, o.pos[2] = f.pos[2] + std::sin(ang) * rr;
        o.pos[1] = scene.groundHeight(float(o.pos[0]), float(o.pos[2]));
        o.rot[0] = f.rot[0] * 180.0 / kPi;
        o.name = "fount" + std::to_string(scene.mission().objects.size());
        o.script = t.script.empty() ? f.script : t.script;
        int idx = scene.addObject(o);
        combat.grow(scene.mission().objects.size());
        world.unitNames.push_back(o.name), world.unitTeams.push_back(o.team);
        ai.addUnit(idx);
        if (!o.script.empty()) scripts.attachScript(o.script, idx);
        return idx;
    };
    auto unitAlive = [&](int o) { return combat.alive(o) && !scene.objectHidden(o); };
    // instant action: waves of hunters
    std::mt19937 rng{uint32_t(SDL_GetTicks())};
    int wave = 0;
    std::vector<int> waveUnits;
    float waveTimer = setup.waves > 0 ? 3.f : 0.f;
    std::string banner;
    float bannerUntil = 0;
    auto spawnWave = [&]() {
        waveUnits.clear();
        Mission& m = scene.missionRW();
        Vec3 c = player.position();
        float base = std::uniform_real_distribution<float>(0, 2 * kPi)(rng);
        for (size_t i = 0; i < setup.enemies.size(); i++) {
            MissionObject o;
            o.type = 0x48;
            o.team = userTeam + 1;
            o.name = "enemy" + std::to_string(wave) + "_" + std::to_string(i + 1);
            Bytes g;
            if (!masterGear(fs, setup.enemies[i].gear.chassis, g)) continue;
            o.defIndex = m.addDef(g.data(), g.size());
            // a group 250-400 m away; closer if that would be off the map
            float ang = base + float(i) * 0.12f, dist = 380.f;
            Vec3 p;
            for (int tries = 0; tries < 24; tries++, dist *= tries % 4 == 3 ? 0.8f : 1.f, ang += 0.4f) {
                p = c + Vec3(std::sin(ang), 0, std::cos(ang)) * dist + Vec3(float(i % 3) * 14.f, 0, float(i / 3) * 14.f);
                const Surface* sf = scene.surfaceAt(p.x, p.z);
                if (p.x > 64 && p.z > 64 && p.x < 4032 && p.z < 4032 && !(sf && sf->damage > 0)) break;  // not into lava
            }
            o.pos[0] = p.x, o.pos[2] = p.z, o.pos[1] = scene.groundHeight(p.x, p.z);
            Vec3 d = c - p;
            o.rot[0] = std::atan2(-d.x, -d.z) / kDeg;
            int idx = scene.addObject(o);
            combat.grow(m.objects.size());
            world.unitNames.push_back(o.name), world.unitTeams.push_back(o.team);
            if (!ai.addUnit(idx)) continue;
            if (!setup.enemies[i].gear.weapons.empty()) combat.setLoadout(idx, setup.enemies[i].gear.weapons);
            if (setup.enemies[i].gear.armor > 0) combat.setStats(idx, setup.enemies[i].gear.armor, setup.enemies[i].gear.fireControl);
            const GearChoice& eg = setup.enemies[i].gear;
            if (eg.hasSystems) combat.setSystems(idx, eg.sensors, eg.maneuver, eg.perks, eg.flaws);
            ai.command(idx, "nonCombatTactic", 0, 0, 0, "hunt");
            waveUnits.push_back(idx);
        }
        ai.setDefcon(userTeam + 1 - 99, 5, 0);
        wave++;
        banner = "WAVE " + std::to_string(wave) + " OF " + std::to_string(setup.waves), bannerUntil = scripts.time() + 4.f;
    };
    // ambient sound objects: buildings / props whose definition names a looping sound (Sounds.lst EAR_PERF_LOOP)
    struct Ambient { int object; std::string wav; float range; int handle = -1; };
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
            if (si && si->loop) ambients.push_back({int(i), g.soundMotor, si->range > 0 ? si->range : 200.f});
        }
    }
    auto updateAmbient = [&]() {
        Vec3 lp = player.position();
        for (auto& a : ambients) {
            Vec3 p;
            bool on = !scene.objectHidden(a.object) && scene.objectPosition(a.object, p) && length(p - lp) < a.range;
            if (on && a.handle < 0) a.handle = audio->startLoop(a.wav, 0.f);
            if (!on && a.handle >= 0) audio->stopLoop(a.handle), a.handle = -1;
            if (a.handle >= 0) audio->setLoop(a.handle, 0.6f * (audio->info(a.wav) ? audio->info(a.wav)->volume : 1.f), &p, a.range);
        }
    };
    Particles particles;
    particles.init(fs, r);
    combat.particles = &particles;
    player.particles = &particles;
    ai.setParticles(&particles);
    world.sceneRW = &scene;
    ai.onEvent = [&](int obj, int ev) { scripts.unitSignal(obj, ev); };
    // engine events from combat: hits (11, 12 while the user shoots), damaged (2), destroyed (5)
    std::map<int, float> lastHitEvent;
    auto drainCombatEvents = [&]() {
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
                    if (combat.ejection(obj)) {  // Ejection System: the pilot gets out and lives to fight again
                        banner = setup.squad[i].name + " EJECTED", bannerUntil = scripts.time() + 3.f;
                    } else {
                        res.squadLost.push_back(setup.squad[i].name);
                        banner = setup.squad[i].name + " IS DOWN", bannerUntil = scripts.time() + 3.f;
                    }
                }
        }
        combat.destroyed.clear();
    };
    world.externalView = &external;
    if (audio) {
        player.audio = audio, player.engineLoop = true;
        ai.setAudio(audio);
    }
    float camDist = 16.f;
    // player-attached camera: cockpit (internal) or chase (external)
    auto viewCamera = [&](Vec3& eye, Vec3& at) {
        if (external) {
            player.chaseCamera(eye, at, camPitch, camOrbit, camDist);
            // keep the chase camera out of walls and hills: pull it in front of the first obstacle
            float t1 = 2.f, t2 = scene.terrainRay(at, eye);
            scene.raycast(at, eye, player.object(), t1);
            float t = std::min(t1, t2);
            if (t < 1.f) eye = at + (eye - at) * std::max(0.08f, t - 0.06f);
        } else {
            player.cockpitCamera(eye, at, camPitch - 0.25f, camOrbit);
            Vec3 pov;  // the model's pilot eye frame follows posture and walk bob
            if (scene.frameWorld(player.object(), "pov", pov)) at += pov - eye, eye = pov;
        }
    };
    auto aimPoint = [&](Vec3& muzzle, Vec3& aim) {
        Vec3 eye, at;
        viewCamera(eye, at);
        Vec3 d = normalize(at - eye);
        if (!combat.muzzle(player.object(), muzzle))
            muzzle = player.position() + Vec3(0, 5.f, 0) + Vec3(-std::sin(player.yaw()), 0, -std::cos(player.yaw())) * 2.f;
        // aim along the camera's heading; mouse pitch above/below the default tilts the shot
        Vec3 h = normalize(Vec3(d.x, 0, d.z));
        aim = muzzle + h * 300.f - Vec3(0, (camPitch - 0.25f) * 300.f, 0);
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
        fp.proj = Mat4::perspective((sniperZoom ? 20.f : 60.f) * kDeg, aspect, 0.5f, 6000.f);
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
    // the user's target: the hostile unit nearest the crosshair (within 10 degrees and 800 m, in sight); squad
    // commands (Attack / Defend My Target) use it, the HUD brackets it
    int userTarget = -1;
    auto pickTarget = [&]() {
        userTarget = -1;
        if (!player.active() || !combat.alive(player.object())) return;
        Vec3 mz, aim;
        aimPoint(mz, aim);
        Vec3 eye = player.position() + Vec3(0, 5, 0), dir = normalize(aim - eye);
        int ptm = scene.mission().objects[size_t(player.object())].team;
        float best = std::cos(10.f * kDeg);
        const auto& objs = scene.mission().objects;
        for (size_t i = 0; i < objs.size(); i++) {
            const auto& o = objs[i];
            if (int(i) == player.object() || !isUnitObjectType(o.type) || o.team == ptm || o.team < 100 || !combat.alive(int(i)) || scene.objectHidden(int(i)))
                continue;
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
    // squad commands: F5 attack my target, F6 defend my target, F7 hold ground, F8 regroup, F9 fire at will,
    // F10 go to the nav point
    auto issueOrder = [&](int k) {
        if (getenv("HG2_TACDBG")) SDL_Log("issueOrder %d squad %d", k, ai.squadSize(player.object()));
        if (ai.squadSize(player.object()) <= 0 || k < 0 || k > 5) return;
        if (!combat.commsOk(player.object())) {
            banner = "COMMS DOWN", bannerUntil = scripts.time() + 1.5f;
            return;
        }
        // squad commands: F5 attack my target, F6 defend my target, F7 hold ground, F8 regroup,
        // F9 fire at will, F10 go to the nav point
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
            // radar: 300 m
            float R = 70 * s, rx = W - R - 16 * s, ry = R + 16 * s;
            float range = 300.f * std::max(0.5f, 1.f + 0.1f * float(combat.sensorRating(player.object())));  // sensors rating
            if (combat.uplink(player.object())) range *= 1.6f;  // Satellite Uplink
            bool sensors = combat.sensorsOk(player.object());
            hud.rect(rx - R, ry - R, 2 * R, 2 * R, dim);
            hud.frame(rx - R, ry - R, 2 * R, 2 * R, green, s);
            hud.rect(rx - 2 * s, ry - 2 * s, 4 * s, 4 * s, green);
            int ptm = scene.mission().objects[size_t(player.object())].team;
            float yw = player.yaw(), cy = std::cos(yw), sy2 = std::sin(yw);
            if (!sensors) hud.text(rx - R + 8 * s, ry - 8 * s, "SENSORS DOWN", red, s);
            int hostiles = 0;
            ai.forEach([&](int, int team, const Vec3& p, bool alive) {
                if (alive && team != ptm) hostiles++;
                if (!alive || !sensors) return;
                Vec3 d = p - player.position();
                float fx = -d.x * sy2 - d.z * cy, rxl = d.z * sy2 - d.x * cy;  // screen right = (-cos, 0, sin)
                float u = rxl / range * R, v = -fx / range * R;
                if (std::fabs(u) > R - 3 * s || std::fabs(v) > R - 3 * s) return;
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
            std::string t = scene.mission().title;
            hud.text(W * 0.5f - hud.textWidth(t, 1.5f * s) * 0.5f, 40 * s, t, amber, 1.5f * s);
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
    auto glowLights = [&]() {
        for (size_t i = 0; i < scene.mission().objects.size(); i++) {  // mission lights glow
            const auto& o = scene.mission().objects[i];
            if (o.isLight && std::find(world.lightsOff.begin(), world.lightsOff.end(), int(i)) == world.lightsOff.end())
                particles.flare(Vec3(float(o.pos[0]), float(o.pos[1]), float(o.pos[2])), o.lightColor, std::max(0.6f, o.lightRange * 0.06f));
        }
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
            if (T.startX >= 0 && f % 3 == 0) {  // test mode: hold position and fire forward
                Vec3 mz, aim;
                aimPoint(mz, aim);
                combat.fire(mz, aim);
            }
            float np[3];
            if (T.navDrive && scripts.checkpointPosition(scripts.selectedCheckpoint, np)) {
                Vec3 d = Vec3(np[0], 0, np[2]) - Vec3(player.position().x, 0, player.position().z);
                float want = std::atan2(-d.x, -d.z), e = want - player.yaw();
                while (e > kPi) e -= 2 * kPi;
                while (e < -kPi) e += 2 * kPi;
                pin.turn = std::max(-1.f, std::min(1.f, e * 2.f));
                pin.throttle = std::fabs(e) < 0.6f && length(d) > 4.f ? 1.f : 0.f;
                pin.run = length(d) > 40.f;
            } else if (T.navDrive) {
                pin = PlayerInput{};
            }
            for (auto& [fr, act] : T.inputScript) {
                if (fr != f) continue;
                if (act == "down") pin.down = true;
                else if (act == "up") pin.up = true;
                else if (act == "jump") pin.jump = true;
                else if (act == "skates") pin.toggleSkates = true;
                else if (act == "view") external = !external;
                else if (act == "flinch") player.hitReaction(3);
                else if (act.rfind("order", 0) == 0) pickTarget(), issueOrder(atoi(act.c_str() + 5));
            }
            if (!world.control) pin = PlayerInput{};
            if (const char* fxn = getenv("HG2_FX"))  // test: an effect 25 m ahead, a few frames before the shot
                if (f == std::max(0, T.driveFrames - atoi(getenv("HG2_FX_AGE") ? getenv("HG2_FX_AGE") : "10"))) {
                    Vec3 eye = frameParams().eye, at = player.position() + (eye - player.position()) * 0.5f;  // between camera and gear
                    at.y = scene.groundHeight(at.x, at.z);
                    printf("effect %s: %s\n", fxn, particles.spawn(fxn, at, 1.5f) ? "ok" : "not found");
                }
            player.speedScale = combat.mobility(player.object()), player.turnScale = combat.manoeuvre(player.object());
            if (player.active() && !combat.alive(player.object())) player.die();
            player.update(1.f / 30.f, pin);
            ai.update(1.f / 30.f);
            combat.update(1.f / 30.f);
            particles.setWeather(world.weatherType, world.weatherPreset % 5);
            particles.setViewer(frameParams().eye);
            particles.update(1.f / 30.f);
            fountains.update(1.f / 30.f, player.position(), spawnUnit, unitAlive);
            if (setup.waves > 0) {
                bool cleared = std::none_of(waveUnits.begin(), waveUnits.end(), [&](int o) { return combat.alive(o); });
                if (cleared && wave < setup.waves && (waveTimer -= 1.f / 30.f) <= 0) spawnWave(), waveTimer = 5.f;
            }
            drainCombatEvents();
            scripts.update(1.f / 30.f);
            if (f % 600 == 0 && getenv("HG2_AIDBG")) ai.dump();
            if (f % 300 == 0 && (T.navDrive || !T.inputScript.empty()))
                printf("t=%5.1f pos %.0f,%.0f v %.1f ctl %d posture %d nav %d %s | %s\n", scripts.time(), player.position().x,
                       player.position().z, player.speed(), int(world.control) + 10 * int(player.forwardLocked()), player.postureCode(),
                       scripts.selectedCheckpoint,
                       scripts.selectedCheckpoint >= 0 ? scripts.checkpoints[size_t(scripts.selectedCheckpoint)].label.c_str() : "-",
                       scripts.screenText.c_str());
        }
        if (T.driveFrames == 0) player.update(0.f, PlayerInput{});
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
        scene.setObjectDrawn(player.object(), external || !chase || scripts.cinematic.active);
        if (T.driveFrames) printf("after %d frames: %s, hostiles alive %d/%d, squad %zu, wave %d, end %d\n", T.driveFrames, combat.status().c_str(),
                                  ai.alive(), ai.count(), mates.size(), wave, world.endCode);
        for (int f = 0; f < T.fireFrames; f++) {
            Vec3 mz, aim;
            aimPoint(mz, aim);
            combat.fire(mz, aim);
            combat.update(1.f / 30.f);
        }
        buildHud(frameParams());
        glowLights();
        particles.submit(frameParams().view);
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
    Uint64 last = SDL_GetTicksNS();
    float deadTime = 0;
    float sens = 0.003f * app.settings.mouseSpeed, inv = app.settings.invertMouse ? -1.f : 1.f;
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
            if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_ESCAPE) setPaused(true);
            else if (ev.type == SDL_EVENT_WINDOW_FOCUS_LOST && !net) setPaused(true);
            else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key >= SDLK_1 && ev.key.key <= SDLK_8)
                combat.selectWeapon(int(ev.key.key - SDLK_1));
            else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_TAB) combat.nextWeapon();
            else if (ev.type == SDL_EVENT_KEY_DOWN && ev.key.key == SDLK_F1 && player.active()) {
                chase = !chase;
                if (!chase) {  // free camera starts where the chase camera was
                    Vec3 eye, at;
                    viewCamera(eye, at);
                    cam.pos = eye;
                    Vec3 d = normalize(at - eye);
                    cam.yaw = std::atan2(d.x, d.z);
                    cam.pitch = std::asin(d.y);
                }
            } else if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
                // original numeric-pad layout plus letter keys: / or Z down, * or X up, Enter or Space jump,
                // Ins (pad 0) or R skates, C internal / external view, N / B next / previous nav point
                SDL_Keycode kc = ev.key.key;
                if (kc == SDLK_KP_DIVIDE || kc == SDLK_Z) edge.down = true;
                else if (kc == SDLK_KP_MULTIPLY || kc == SDLK_X) edge.up = true;
                else if ((kc == SDLK_KP_ENTER || kc == SDLK_SPACE) && scripts.cinematic.active) scripts.skipCinematic();
                else if (kc == SDLK_KP_ENTER || kc == SDLK_SPACE) edge.jump = chase;
                else if (kc == SDLK_KP_0 || kc == SDLK_INSERT || kc == SDLK_R) edge.toggleSkates = true;
                else if (kc == SDLK_C) external = !external, app.settings.externalView = external;
                else if (kc == SDLK_N) scripts.stepCheckpoint(1);
                else if (kc == SDLK_M && audio) audio->musicVolume = audio->musicVolume > 0 ? 0.f : app.settings.music;
                else if (kc == SDLK_B) scripts.stepCheckpoint(-1);
                else if (kc >= SDLK_F5 && kc <= SDLK_F10) issueOrder(int(kc - SDLK_F5));
                else if (kc == SDLK_V && combat.sniper(player.object())) sniperZoom = !sniperZoom;  // Sniper Systems zoom
            } else if (ev.type == SDL_EVENT_MOUSE_WHEEL) {
                camDist = std::max(8.f, std::min(60.f, camDist - ev.wheel.y * 2.f));
            } else if (ev.type == SDL_EVENT_MOUSE_MOTION) {
                if (chase) {
                    camOrbit += ev.motion.xrel * sens;
                    camPitch = std::max(-0.2f, std::min(1.3f, camPitch + ev.motion.yrel * sens * inv));
                } else {
                    cam.yaw += ev.motion.xrel * sens;
                    cam.pitch = std::max(-1.5f, std::min(1.5f, cam.pitch - ev.motion.yrel * sens * inv));
                }
            }
        }
        Uint64 now = SDL_GetTicksNS();
        float dt = std::min(float(now - last) * 1e-9f, 0.1f);
        last = now;
        if (paused) {
            if (net) net->poll();  // keep the connection alive; the world goes on without us
            FrameParams fp = frameParams();
            buildHud(fp);
            glowLights();
            particles.submit(fp.view);
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
        if (chase) {
            pin.throttle = (k[SDL_SCANCODE_W] || k[SDL_SCANCODE_KP_8] ? 1.f : 0.f) - (k[SDL_SCANCODE_S] || k[SDL_SCANCODE_KP_2] ? 1.f : 0.f);
            pin.turn = (k[SDL_SCANCODE_D] || k[SDL_SCANCODE_KP_6] ? 1.f : 0.f) - (k[SDL_SCANCODE_A] || k[SDL_SCANCODE_KP_4] ? 1.f : 0.f);
            pin.strafe = (k[SDL_SCANCODE_E] || k[SDL_SCANCODE_KP_9] ? 1.f : 0.f) - (k[SDL_SCANCODE_Q] || k[SDL_SCANCODE_KP_7] ? 1.f : 0.f);
            pin.run = k[SDL_SCANCODE_LSHIFT];
            pin.down = edge.down, pin.up = edge.up, pin.jump = edge.jump, pin.toggleSkates = edge.toggleSkates;
            // arrow keys orbit / tilt the camera, Home / End zoom the external camera
            camOrbit += ((k[SDL_SCANCODE_RIGHT] ? 1.f : 0.f) - (k[SDL_SCANCODE_LEFT] ? 1.f : 0.f)) * 1.5f * dt;
            camPitch = std::max(-0.2f, std::min(1.3f, camPitch + ((k[SDL_SCANCODE_DOWN] ? 1.f : 0.f) - (k[SDL_SCANCODE_UP] ? 1.f : 0.f)) * dt));
            if (k[SDL_SCANCODE_HOME]) camDist = std::max(8.f, camDist - 20.f * dt);
            if (k[SDL_SCANCODE_END]) camDist = std::min(60.f, camDist + 20.f * dt);
            bool steering = k[SDL_SCANCODE_LEFT] || k[SDL_SCANCODE_RIGHT];
            if (!steering) camOrbit *= std::max(0.f, 1.f - dt * (external ? 1.5f : 0.6f));  // drift back to the gear's heading
        }
        bool dead = player.active() && !combat.alive(player.object());
        if (dead) player.die();
        if (dead || !world.control) pin = PlayerInput{};
        player.twist = chase ? std::max(-1.2f, std::min(1.2f, camOrbit)) : 0.f;  // torso follows the aim
        edge = PlayerInput{};
        player.speedScale = combat.mobility(player.object()), player.turnScale = combat.manoeuvre(player.object());
        player.jumpJets = combat.jumpJets(player.object());
        combat.setPosture(player.object(), player.postureCode());
        if (botClose && !dead) pin.throttle = 1;
        player.update(dt, pin);
        scene.setObjectDrawn(player.object(), external || !chase || scripts.cinematic.active);
        ai.update(dt);
        if (audio) {
            if (chase) {
                Vec3 eye, at;
                viewCamera(eye, at);
                audio->setListener(eye, std::atan2(-(at - eye).x, -(at - eye).z));
            } else {
                audio->setListener(cam.pos, std::atan2(-cam.forward().x, -cam.forward().z));
            }
            audio->update();
            static float ambT = 0;
            if ((ambT += dt) > 0.25f) ambT = 0, updateAmbient();
        }
        pickTarget();
        if (player.active() && combat.designator(player.object()))
            combat.setDesignated(userTarget, scene.mission().objects[size_t(player.object())].team);
        bool fired = false;
        Vec3 fmz, faim;
        if (chase && !dead && world.control && ((SDL_GetMouseState(nullptr, nullptr) & SDL_BUTTON_LMASK) || k[SDL_SCANCODE_LCTRL])) {
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
        particles.setWeather(world.weatherType, world.weatherPreset % 5);
        particles.setViewer(frameParams().eye);
        particles.update(dt);
        fountains.update(dt, player.position(), spawnUnit, unitAlive);
        if (setup.waves > 0 && world.endCode < 0) {
            bool cleared = std::none_of(waveUnits.begin(), waveUnits.end(), [&](int o) { return combat.alive(o); });
            if (cleared && (waveTimer -= dt) <= 0) {
                if (wave < setup.waves) spawnWave(), waveTimer = 5.f;
                else world.missionOver(1, 4.f);
            }
        }
        drainCombatEvents();
        if (getenv("HG2_AIDBG") && int(scripts.time() / 10) != int((scripts.time() - dt) / 10)) ai.dump();
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
        glowLights();
        particles.submit(fp.view);
        if (r.beginFrame(fp)) {
            scene.render(frameParams());
            combat.draw();
            r.drawDynamic();
            r.endFrame();
        }
    }
}

}  // namespace hg2
