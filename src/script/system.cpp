#include "system.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

namespace hg2 {

namespace {
// Builtins read their arguments below the stack top: the last argument is at top-4.
int32_t argI(const uint8_t* top, int byteOff) {
    int32_t v;
    memcpy(&v, top - byteOff, 4);
    return v;
}
float argF(const uint8_t* top, int byteOff) {
    float v;
    memcpy(&v, top - byteOff, 4);
    return v;
}
ScriptSystem* g_sys = nullptr;

// Unit references (FUN_100d3480): 0 and 6 = the user's gear, 4 = the unit running the script; 1 = enemy in its
// notify range, 2 = friend in range, 3 = focus unit, 5 = attacker; 7..0x17 = the user's squad mates.
int unitRef(ScriptVM& vm, int ref) {
    if (ref == 0 || ref == 6) return g_sys->world().playerUnit();
    if (ref == 4) return vm.owner();
    if (ref == 3) {
        auto it = g_sys->focusOf(&vm);
        if (it >= 0) return it;
    }
    if (ref >= 1 && ref <= 0x17) return g_sys->world().unitRelation(vm.owner(), ref);
    return -1;
}
int unitName(ScriptVM& vm, const uint8_t* t, int off) { return g_sys->world().findUnit(vm.strings().get(argI(t, off))); }
}  // namespace

int ScriptWorld::findUnit(const std::string& name) {
    // Units are looked up by their first 8 characters, case-insensitively (FUN_100bc740 callers).
    std::string k = lower(name.substr(0, 8));
    for (size_t i = 0; i < unitNames.size(); i++)
        if (lower(unitNames[i].substr(0, 8)) == k) return int(i);
    return -1;
}
// Mission objects carry teams 99 (neutral), 100 (the user's side), 101…; scripts number them from 0 (team - 99).
int ScriptWorld::teamOf(int unit) {
    if (unit < 0 || size_t(unit) >= unitTeams.size()) return -1;
    int t = unitTeams[size_t(unit)];
    return t >= 99 ? t - 99 : t;
}

ScriptSystem::ScriptSystem(const FileSystem& fs, ScriptWorld& world) : fs_(fs), world_(world) {
    g_sys = this;
    bindBuiltins();
}

void ScriptSystem::log(const std::string& s) {
    if (verbose) SDL_Log("[script] %s", s.c_str());
}

void ScriptSystem::unimplemented(ScriptVM& vm, const std::string& name, int overload) {
    std::string k = name + "/" + std::to_string(overload);
    if (missing.insert(k).second && verbose) SDL_Log("[script] %s: builtin %s not implemented yet", vm.where().c_str(), k.c_str());
}

uint64_t ScriptSystem::instructions() const {
    uint64_t n = 0;
    for (auto& i : vms_) n += i.vm->executed;
    return n;
}

std::set<std::string> ScriptSystem::calledNatives(bool unboundOnly) const {
    // Every program declares the whole builtin table; only symbols named by a call instruction
    // (op 2 via symbol, op 3 linked native) are actually used.
    std::set<std::string> out;
    for (auto& p : programs_)
        for (auto& in : p->code) {
            if ((in.op != 2 && in.op != 3) || !in.operand) continue;
            int32_t sym;
            memcpy(&sym, in.operand, 4);
            if (sym < 0 || size_t(sym) >= p->symbols.size()) continue;
            const auto& s = p->symbols[size_t(sym)];
            if (s.kind != 1 || (s.resolved && !s.native)) continue;
            const std::string& nm = strings_.get(s.name);
            if (!unboundOnly || !ScriptVM::isBound(nm, s.overload)) out.insert(nm + "/" + std::to_string(s.overload));
        }
    return out;
}

std::shared_ptr<ScriptProgram> ScriptSystem::program(const std::string& name, std::string* err) {
    std::string base = name;
    size_t dot = base.rfind('.');
    if (dot != std::string::npos) base = base.substr(0, dot);
    for (auto& p : programs_)
        if (lower(p->name) == lower(base)) return p;
    Bytes b;
    if (!fs_.read("scripting/" + base + ".o", b) && !fs_.read(base + ".o", b)) {
        if (err) *err = "script not found: " + base + ".o";
        return nullptr;
    }
    auto p = std::make_shared<ScriptProgram>();
    if (!p->load(b, base, strings_, err)) return nullptr;
    programs_.push_back(p);
    ScriptVM::exportFunctions(p);
    return p;
}

bool ScriptSystem::load(const Mission& m, std::string* err) {
    ScriptVM::resetGlobals();
    vms_.clear();
    timers_.clear();
    // Library scripts from the mission's script projects are loaded first so their exported
    // globals exist when unit and mission scripts resolve externs (FUN_100c7450).
    for (auto& prj : m.scriptProjects) {
        Bytes pb;
        if (!fs_.read("scripting/" + prj, pb)) continue;
        std::string s(pb.begin(), pb.end());
        size_t a = s.find("Source_Files"), b = a == std::string::npos ? a : s.find('}', a);
        if (a == std::string::npos || b == std::string::npos) continue;
        for (size_t q = s.find('"', a); q != std::string::npos && q < b; q = s.find('"', q + 1)) {
            size_t e = s.find('"', q + 1);
            if (e == std::string::npos) break;
            std::string src = s.substr(q + 1, e - q - 1);
            q = e;
            std::string perr;
            if (!program(src, &perr)) log(perr);
        }
    }
    auto attach = [&](const std::string& file, int unit) {
        std::string perr;
        auto p = program(file, &perr);
        if (!p) {
            log(perr);
            return;
        }
        vms_.push_back({std::make_unique<ScriptVM>(p, *this, strings_, unit), unit});
    };
    for (auto& f : m.scriptFiles) attach(f, -1);
    for (size_t i = 0; i < m.objects.size(); i++)
        if (!m.objects[i].script.empty()) attach(m.objects[i].script, int(i));
    mission_ = &m;
    cinematic = Cinematic{};
    // UserMessageFile: numbered lines "N :TEXT"
    messages_.clear();
    if (!m.msgFile.empty()) {
        Bytes mb;
        if (fs_.read("mission/" + m.msgFile, mb) || fs_.read(m.msgFile, mb)) {
            std::string txt(mb.begin(), mb.end()), line;
            size_t a = 0;
            while (a < txt.size()) {
                size_t e = txt.find('\n', a);
                if (e == std::string::npos) e = txt.size();
                line = txt.substr(a, e - a);
                a = e + 1;
                while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
                size_t c = line.find(':');
                if (c == std::string::npos) continue;
                int n = atoi(line.c_str());
                if (n > 0) messages_[n] = line.substr(c + 1);
            }
        }
    }
    for (auto& o : objectives) o = Objective{};
    for (auto& c : checkpoints) c = Checkpoint{};
    selectedCheckpoint = -1;
    cinematicMode = false;
    allowCinematicCombat = false;
    fade = Fade{};
    fadeHold_ = false;
    focus_.clear();
    // event 0 is the start-up handler
    for (auto& i : vms_) i.vm->signal(0);
    (void)err;
    return true;
}

void ScriptSystem::attachScript(const std::string& file, int unit) {
    std::string perr;
    auto p = program(file, &perr);
    if (!p) {
        log(perr);
        return;
    }
    vms_.push_back({std::make_unique<ScriptVM>(p, *this, strings_, unit), unit});
    vms_.back().vm->signal(0);
}

void ScriptSystem::setTimer(ScriptVM& vm, float seconds, int sig) {
    if (seconds <= 0) return;
    timers_.push_back({&vm, time_ + seconds, sig & 0x3f});
}

void ScriptSystem::unitSignal(int unit, int sig) {
    for (auto& i : vms_)
        if (i.unit == unit && unit >= 0) {
            if (verbose && sig != 11 && sig != 12) SDL_Log("[script] event %d -> %s", sig, i.vm->where().c_str());
            i.vm->signal(sig);
        }
}
void ScriptSystem::teamSignal(int team, int sig) {
    for (auto& i : vms_)
        if (i.unit >= 0 && world_.teamOf(i.unit) == team) i.vm->signal(sig);
}
void ScriptSystem::missionSignal(int sig) {
    for (auto& i : vms_)
        if (i.unit < 0) i.vm->signal(sig);
}
void ScriptSystem::worldSignal(int sig) {
    for (auto& i : vms_) i.vm->signal(sig);
}

namespace {
void samplePath(const MissionPath& p, float f, float out[3]) {
    const auto& pts = p.points;
    if (pts.empty()) {
        for (int k = 0; k < 3; k++) out[k] = p.anchor[k];
        return;
    }
    std::vector<float> acc(pts.size(), 0.f);
    for (size_t i = 1; i < pts.size(); i++) {
        float dx = pts[i][0] - pts[i - 1][0], dy = pts[i][1] - pts[i - 1][1], dz = pts[i][2] - pts[i - 1][2];
        acc[i] = acc[i - 1] + std::sqrt(dx * dx + dy * dy + dz * dz);
    }
    float L = acc.back() * std::max(0.f, std::min(1.f, f));
    size_t i = 1;
    while (i < pts.size() && acc[i] < L) i++;
    if (i >= pts.size()) {
        for (int k = 0; k < 3; k++) out[k] = pts.back()[size_t(k)];
        return;
    }
    float seg = acc[i] - acc[i - 1], u = seg > 0 ? (L - acc[i - 1]) / seg : 0.f;
    for (int k = 0; k < 3; k++) out[k] = pts[i - 1][size_t(k)] + (pts[i][size_t(k)] - pts[i - 1][size_t(k)]) * u;
}
}  // namespace

void ScriptSystem::startCinematic(ScriptVM& vm, const std::string& path, int endSig, int skipSig, float duration) {
    if (endSig > 0x3f) endSig = -1;
    if (skipSig > 0x3f) skipSig = -1;
    const MissionPath* p = mission_ ? mission_->path(path) : nullptr;
    if (!p) {
        log("camera path " + path + " does not exist");
        if (endSig >= 0) vm.signal(endSig);
        return;
    }
    cinematic = Cinematic{};
    cinematic.active = true, cinematic.path = p, cinematic.vm = &vm;
    cinematic.endSig = endSig, cinematic.skipSig = skipSig;
    cinematic.duration = duration > 0 ? duration : 5.f;
    if (verbose)
        SDL_Log("[script] camera on path %s (%zu points, %.0f,%.0f,%.0f .. %.0f,%.0f,%.0f) for %.1fs (end signal %d)", path.c_str(),
                p->points.size(), p->points.front()[0], p->points.front()[1], p->points.front()[2], p->points.back()[0],
                p->points.back()[1], p->points.back()[2], cinematic.duration, endSig);
}

bool ScriptSystem::cinematicCamera(float eye[3], float at[3]) {
    if (!cinematic.active || !cinematic.path) return false;
    float f = cinematic.t / cinematic.duration;
    samplePath(*cinematic.path, f, eye);
    float u[3];
    if (cinematic.lookUnit >= 0 && world_.unitPosition(cinematic.lookUnit, u)) {
        at[0] = u[0], at[1] = u[1] + 4.f, at[2] = u[2];
    } else if (cinematic.hasLookPoint) {
        for (int k = 0; k < 3; k++) at[k] = cinematic.lookPoint[k];
    } else if (cinematic.lookPath) {
        samplePath(*cinematic.lookPath, f, at);
    } else {
        samplePath(*cinematic.path, std::min(1.f, f + 0.05f), at);
        if (std::fabs(at[0] - eye[0]) + std::fabs(at[2] - eye[2]) < 0.1f) at[0] += 1.f;
    }
    return true;
}

void ScriptSystem::skipCinematic() {
    if (!cinematic.active) return;
    cinematic.active = false;
    int sig = cinematic.skipSig >= 0 ? cinematic.skipSig : cinematic.endSig;
    if (sig >= 0 && cinematic.vm) cinematic.vm->signal(sig);
}

int ScriptSystem::focusOf(ScriptVM* vm) const {
    auto it = focus_.find(vm);
    return it == focus_.end() ? -1 : it->second.target;
}

float ScriptSystem::fadeAlpha() const {
    if (!fade.active && !fadeHold_) return 0.f;
    float u = fade.active ? std::min(1.f, fade.t / std::max(0.05f, fade.time)) : 1.f;
    return fade.active ? (fade.in ? 1.f - u : u) : 1.f;
}

const std::string& ScriptSystem::message(int index) const {
    static const std::string empty;
    auto it = messages_.find(index);
    return it == messages_.end() ? empty : it->second;
}

bool ScriptSystem::checkpointPosition(int id, float out[3]) {
    if (id < 0 || id >= 26 || !checkpoints[id].active) return false;
    const Checkpoint& c = checkpoints[id];
    for (int k = 0; k < 3; k++) out[k] = c.pos[k];
    float u[3];
    // a checkpoint given with a unit and no position follows the unit
    if (c.unit >= 0 && c.pos[0] == 0 && c.pos[2] == 0 && world_.unitPosition(c.unit, u))
        for (int k = 0; k < 3; k++) out[k] = u[k];
    return true;
}

void ScriptSystem::selectCheckpoint(int id) {
    if (id >= 0 && id < 26 && checkpoints[id].active) selectedCheckpoint = id;
}

void ScriptSystem::stepCheckpoint(int dir) {
    for (int k = 1; k <= 26; k++) {
        int id = ((selectedCheckpoint < 0 ? 0 : selectedCheckpoint) + dir * k + 26 * 2) % 26;
        if (checkpoints[id].active) {
            selectedCheckpoint = id;
            return;
        }
    }
}

void ScriptSystem::update(float dt) {
    dt_ = dt;
    time_ += dt;
    if (fade.active && (fade.t += dt) >= fade.time) {
        fade.active = false;
        fadeHold_ = !fade.in;  // stays black after a fade out
        if (fade.signal >= 0 && fade.vm) fade.vm->signal(fade.signal);
    }
    if (cinematic.active && (cinematic.t += dt) >= cinematic.duration) {
        cinematic.active = false;
        if (cinematic.endSig >= 0 && cinematic.vm) cinematic.vm->signal(cinematic.endSig);
    }
    // focus notification: event 23 on the watching script when its focus unit comes within range
    for (auto& [vm, f] : focus_) {
        if (f.target < 0 || f.range <= 0 || vm->terminated()) continue;
        float a[3], b[3];
        if (vm->owner() < 0 || !world_.unitPosition(vm->owner(), a) || !world_.unitPosition(f.target, b)) continue;
        float dx = a[0] - b[0], dz = a[2] - b[2];
        bool in = dx * dx + dz * dz <= f.range * f.range;
        if (in != f.inside) vm->signal(in ? 23 : 25);  // focus entered / left the range
        f.inside = in;
    }
    for (size_t k = 0; k < timers_.size();) {
        if (timers_[k].at <= time_) {
            timers_[k].vm->signal(timers_[k].sig);
            timers_.erase(timers_.begin() + long(k));
        } else {
            k++;
        }
    }
    for (auto& i : vms_) {
        current_ = i.vm.get();
        i.vm->tick();
    }
    current_ = nullptr;
    // A unit's script is dropped once the unit is dead and its handlers (the death event among them) have run,
    // unless the script made itself Persistent (FUN_100ba4e0 loop: brain +0x194 bit 0). The user's gear keeps its
    // script (the mission ends with it anyway, and network play revives it). One update of grace lets the death
    // event raised after this frame's tick still reach the script.
    int user = world_.playerUnit();
    for (auto& i : vms_) {
        if (i.unit < 0 || i.unit == user || i.vm->persistent || i.vm->terminated()) continue;
        if (world_.isAlive(i.unit) || i.vm->pending()) {
            i.deadTicks = 0;
            continue;
        }
        if (++i.deadTicks >= 2) {
            if (verbose) SDL_Log("[script] %s: unit %d dead, script dropped", i.vm->where().c_str(), i.unit);
            i.vm->terminate();
        }
    }
}

// ------------------------------------------------------------------ builtins
void ScriptSystem::bindBuiltins() {
    using V = ScriptVM;
    auto fret = [](ScriptValue& r, float v) { r.setFloat(v); };
    V::bind("hg_sin", 0, [=](V&, const uint8_t* t, ScriptValue& r) { fret(r, std::sin(argF(t, 4))); });
    V::bind("hg_cos", 0, [=](V&, const uint8_t* t, ScriptValue& r) { fret(r, std::cos(argF(t, 4))); });
    V::bind("hg_tan", 0, [=](V&, const uint8_t* t, ScriptValue& r) { fret(r, std::tan(argF(t, 4))); });
    V::bind("hg_atan", 0, [=](V&, const uint8_t* t, ScriptValue& r) { fret(r, std::atan2(argF(t, 4), 1.f)); });
    V::bind("hg_atan", 1, [=](V&, const uint8_t* t, ScriptValue& r) { fret(r, std::atan2(argF(t, 4), argF(t, 8))); });
    V::bind("hg_rand", 0, [=](V&, const uint8_t*, ScriptValue& r) { fret(r, float(rand()) / float(RAND_MAX)); });
    V::bind("VectorScale", 0, [](V&, const uint8_t* t, ScriptValue& r) {
        float s = argF(t, 4);
        r.setVec(argF(t, 16) * s, argF(t, 12) * s, argF(t, 8) * s);
    });
    V::bind("VectorSum", 0, [=](V&, const uint8_t* t, ScriptValue& r) { fret(r, argF(t, 12) + argF(t, 8) + argF(t, 4)); });
    V::bind("VectorMag", 0, [=](V&, const uint8_t* t, ScriptValue& r) {
        float x = argF(t, 12), y = argF(t, 8), z = argF(t, 4);
        fret(r, std::sqrt(x * x + y * y + z * z));
    });
    V::bind("VectorAng", 0, [=](V&, const uint8_t* t, ScriptValue& r) {
        fret(r, std::atan2(argF(t, 24), argF(t, 16)) - std::atan2(argF(t, 12), argF(t, 4)));
    });
    V::bind("GetVectorX", 0, [=](V&, const uint8_t* t, ScriptValue& r) { fret(r, argF(t, 12)); });
    V::bind("GetVectorY", 0, [=](V&, const uint8_t* t, ScriptValue& r) { fret(r, argF(t, 8)); });
    V::bind("GetVectorZ", 0, [=](V&, const uint8_t* t, ScriptValue& r) { fret(r, argF(t, 4)); });
    V::bind("SetVectorX", 0, [](V&, const uint8_t* t, ScriptValue& r) { r.setVec(argF(t, 4), argF(t, 12), argF(t, 8)); });
    V::bind("SetVectorY", 0, [](V&, const uint8_t* t, ScriptValue& r) { r.setVec(argF(t, 16), argF(t, 4), argF(t, 8)); });
    V::bind("SetVectorZ", 0, [](V&, const uint8_t* t, ScriptValue& r) { r.setVec(argF(t, 16), argF(t, 12), argF(t, 4)); });
    V::bind("EnableState", 0, [](V& vm, const uint8_t* t, ScriptValue&) { vm.enableState(argI(t, 4), true); });
    V::bind("DisableState", 0, [](V& vm, const uint8_t* t, ScriptValue&) { vm.enableState(argI(t, 4), false); });
    V::bind("WorldSignal", 0, [](V&, const uint8_t* t, ScriptValue&) { g_sys->worldSignal(argI(t, 4)); });
    V::bind("MissionSignal", 0, [](V&, const uint8_t* t, ScriptValue&) { g_sys->missionSignal(argI(t, 4)); });
    V::bind("TeamSignal", 0, [](V& vm, const uint8_t* t, ScriptValue&) {
        g_sys->teamSignal(g_sys->world().teamOf(vm.owner()), argI(t, 4));
    });
    V::bind("TeamSignal", 1, [](V&, const uint8_t* t, ScriptValue&) { g_sys->teamSignal(argI(t, 8), argI(t, 4)); });
    V::bind("UnitSignal", 0, [](V& vm, const uint8_t* t, ScriptValue&) {
        g_sys->unitSignal(unitRef(vm, argI(t, 8)), argI(t, 4));
    });
    V::bind("UnitSignal", 1, [](V& vm, const uint8_t* t, ScriptValue&) {
        g_sys->unitSignal(g_sys->world().findUnit(vm.strings().get(argI(t, 8))), argI(t, 4));
    });
    V::bind("GetTimeStep", 0, [=](V&, const uint8_t*, ScriptValue& r) { fret(r, g_sys->timeStep()); });
    V::bind("Terminate", 0, [](V& vm, const uint8_t*, ScriptValue&) { vm.terminate(); });
    V::bind("Persistent", 0, [](V&, const uint8_t*, ScriptValue&) {});
    V::bind("SetTimer", 0, [](V& vm, const uint8_t* t, ScriptValue&) { g_sys->setTimer(vm, argF(t, 8), argI(t, 4)); });
    V::bind("SetTimer", 1, [](V& vm, const uint8_t* t, ScriptValue&) {
        g_sys->setTimer(vm, float(argI(t, 8)) / 30.f, argI(t, 4));  // frame count
    });
    V::bind("UserAlive", 0, [](V&, const uint8_t*, ScriptValue& r) { r.setInt(g_sys->world().userAlive()); });
    V::bind("IsAlive", 0, [](V& vm, const uint8_t* t, ScriptValue& r) {
        r.setInt(g_sys->world().isAlive(unitRef(vm, argI(t, 4))));
    });
    V::bind("IsAlive", 1, [](V& vm, const uint8_t* t, ScriptValue& r) {
        r.setInt(g_sys->world().isAlive(g_sys->world().findUnit(vm.strings().get(argI(t, 4)))));
    });
    V::bind("MissionOver", 0, [](V&, const uint8_t* t, ScriptValue&) { g_sys->world().missionOver(argI(t, 4), 0.f); });
    V::bind("MissionOver", 1, [](V&, const uint8_t* t, ScriptValue&) { g_sys->world().missionOver(argI(t, 8), argF(t, 4)); });
    V::bind("TriggerGlobalSound", 0, [](V& vm, const uint8_t* t, ScriptValue&) {
        g_sys->world().playSound(vm.strings().get(argI(t, 4)), true);
    });
    // ---- unit orders
    // AutoSleep(unit, allowed): whether the unit dozes off when far from the user and out of view (FUN_10049a10)
    V::bind("AutoSleep", 0, [](V& vm, const uint8_t* t, ScriptValue&) { g_sys->world().unitCommand(unitRef(vm, argI(t, 8)), "autoSleep", float(argI(t, 4))); });
    V::bind("AutoSleep", 1, [](V& vm, const uint8_t* t, ScriptValue&) { g_sys->world().unitCommand(unitName(vm, t, 8), "autoSleep", float(argI(t, 4))); });
    V::bind("ActivateUnit", 0, [](V& vm, const uint8_t* t, ScriptValue&) { g_sys->world().unitActivate(unitRef(vm, argI(t, 8)), argI(t, 4) != 0); });
    V::bind("ActivateUnit", 1, [](V& vm, const uint8_t* t, ScriptValue&) { g_sys->world().unitActivate(unitName(vm, t, 8), argI(t, 4) != 0); });
    V::bind("FollowPath", 0, [](V& vm, const uint8_t* t, ScriptValue&) {
        float p[3] = {argF(t, 16), argF(t, 12), argF(t, 8)};
        g_sys->world().unitGoto(vm.owner(), p);
    });
    for (int ov : {1, 2, 3, 4, 5})  // path-name forms; the name is the first string argument found
        V::bind("FollowPath", ov, [](V& vm, const uint8_t* t, ScriptValue&) {
            for (int off : {4, 8, 12})
                if (g_sys->world().unitFollowPath(vm.owner(), vm.strings().get(argI(t, off)))) return;
        });
    V::bind("AssignPath", 0, [](V& vm, const uint8_t* t, ScriptValue&) { g_sys->world().unitAssignPath(vm.owner(), vm.strings().get(argI(t, 4))); });
    V::bind("FullBrake", 0, [](V& vm, const uint8_t* t, ScriptValue&) { g_sys->world().unitStop(unitRef(vm, argI(t, 4))); });
    V::bind("FullBrake", 1, [](V& vm, const uint8_t* t, ScriptValue&) { g_sys->world().unitStop(unitName(vm, t, 4)); });
    V::bind("SetSpeed", 0, [](V& vm, const uint8_t* t, ScriptValue&) { g_sys->world().unitSpeed(vm.owner(), argF(t, 4)); });
    V::bind("Attack", 0, [](V& vm, const uint8_t* t, ScriptValue&) { g_sys->world().unitAttack(vm.owner(), unitRef(vm, argI(t, 4))); });
    V::bind("Attack", 1, [](V& vm, const uint8_t* t, ScriptValue&) { g_sys->world().unitAttack(vm.owner(), unitName(vm, t, 4)); });
    V::bind("SetEnemyNotifyRange", 0, [](V& vm, const uint8_t* t, ScriptValue&) { g_sys->world().unitNotifyRange(vm.owner(), argF(t, 4)); });
    V::bind("AutoPilot", 0, [](V& vm, const uint8_t*, ScriptValue&) { g_sys->world().unitAutoPilot(vm.owner()); });
    V::bind("SetUserControl", 0, [](V&, const uint8_t* t, ScriptValue&) { g_sys->world().userControl(argI(t, 4) != 0); });
    for (int ov : {0, 1})
        V::bind("QueueDialog", ov, [](V& vm, const uint8_t* t, ScriptValue&) { g_sys->world().queueDialog(vm.strings().get(argI(t, 8))); });
    V::bind("QueueDialog", 2, [](V& vm, const uint8_t* t, ScriptValue&) { g_sys->world().queueDialog(vm.strings().get(argI(t, 12))); });
    // ControlCamera: ov0 ends the cinematic; the others share (-4 duration, -8 end signal, -0xc skip signal).
    // ControlCamera() (FUN_100d5ef0 -> FUN_10092ff0(0)): back to the cockpit view, which also ends cinematic mode (+0x521)
    V::bind("ControlCamera", 0, [](V&, const uint8_t*, ScriptValue&) {
        g_sys->cinematic.active = false, g_sys->cinematicMode = false;
        if (g_sys->verbose) SDL_Log("[script] camera back to the user");
    });
    V::bind("ControlCamera", 1, [](V& vm, const uint8_t* t, ScriptValue&) {
        g_sys->startCinematic(vm, vm.strings().get(argI(t, 0x10)), argI(t, 8), argI(t, 0xc), argF(t, 4));
    });
    V::bind("ControlCamera", 2, [](V& vm, const uint8_t* t, ScriptValue&) {
        g_sys->startCinematic(vm, vm.strings().get(argI(t, 0x1c)), argI(t, 8), argI(t, 0xc), argF(t, 4));
        auto& c = g_sys->cinematic;
        c.hasLookPoint = true, c.lookPoint[0] = argF(t, 0x18), c.lookPoint[1] = argF(t, 0x14), c.lookPoint[2] = argF(t, 0x10);
    });
    V::bind("ControlCamera", 3, [](V& vm, const uint8_t* t, ScriptValue&) {
        g_sys->startCinematic(vm, vm.strings().get(argI(t, 0x14)), argI(t, 8), argI(t, 0xc), argF(t, 4));
        g_sys->cinematic.lookUnit = unitName(vm, t, 0x10);
    });
    V::bind("ControlCamera", 4, [](V& vm, const uint8_t* t, ScriptValue&) {
        g_sys->startCinematic(vm, vm.strings().get(argI(t, 0x14)), argI(t, 8), argI(t, 0xc), argF(t, 4));
        g_sys->cinematic.lookUnit = unitRef(vm, argI(t, 0x10));
    });
    V::bind("ControlCamera", 5, [](V& vm, const uint8_t* t, ScriptValue&) {
        g_sys->startCinematic(vm, vm.strings().get(argI(t, 0x18)), argI(t, 8), argI(t, 0xc), argF(t, 4));
        if (g_sys->cinematic.active && g_sys->mission())
            g_sys->cinematic.lookPath = g_sys->mission()->path(vm.strings().get(argI(t, 0x10)));
    });
    // SetCinematicControl(unit, on) (FUN_100d66d0 / FUN_100d6710): brain +0x198, the unit only does what its script
    // orders (AutoPilot clears it); SetCinematicControl(on) (FUN_100d67b0 -> FUN_10092f70) is the global cinematic mode.
    V::bind("SetCinematicControl", 0, [](V& vm, const uint8_t* t, ScriptValue&) {
        g_sys->world().unitCommand(unitRef(vm, argI(t, 8)), "cinematicControl", float(argI(t, 4) != 0));
    });
    V::bind("SetCinematicControl", 1, [](V& vm, const uint8_t* t, ScriptValue&) {
        g_sys->world().unitCommand(unitName(vm, t, 8), "cinematicControl", float(argI(t, 4) != 0));
    });
    V::bind("SetCinematicControl", 2, [](V&, const uint8_t* t, ScriptValue&) {
        g_sys->cinematicMode = argI(t, 4) != 0;
        if (g_sys->verbose) SDL_Log("[script] cinematic mode %d", int(g_sys->cinematicMode));
    });
    // SetWeaponConstraints(unit, pitch-, pitch+, yaw-, yaw+) in degrees (FUN_100d6b90 / FUN_100d6c10 -> vtable +0x1a4,
    // which only the turret class implements, FUN_100ec400: pitch joint (-a, b), yaw joint (-c, d)). Yaw only here.
    V::bind("SetWeaponConstraints", 0, [](V& vm, const uint8_t* t, ScriptValue&) {
        g_sys->world().unitCommand(unitRef(vm, argI(t, 0x14)), "weaponArc", argF(t, 8), argF(t, 4));
    });
    V::bind("SetWeaponConstraints", 1, [](V& vm, const uint8_t* t, ScriptValue&) {
        g_sys->world().unitCommand(unitName(vm, t, 0x14), "weaponArc", argF(t, 8), argF(t, 4));
    });
    // ---- brain settings and unit orders (0.7)
    auto cmd = [](int unit, const char* c, float a = 0, float b = 0, float cc = 0, float d = 0, const std::string& str = std::string()) {
        if (unit >= 0) g_sys->world().unitCommand(unit, c, a, b, cc, d, str);
    };
    V::bind("SetVisibilityRange", 0, [=](V& vm, const uint8_t* t, ScriptValue&) { cmd(vm.owner(), "visibility", argF(t, 4)); });
    V::bind("SetVisibilityRange", 1, [=](V& vm, const uint8_t* t, ScriptValue&) { cmd(unitName(vm, t, 8), "visibility", argF(t, 4)); });
    V::bind("SetFriendlyNotifyRange", 0, [=](V& vm, const uint8_t* t, ScriptValue&) { cmd(vm.owner(), "friendRange", argF(t, 4)); });
    V::bind("CanTarget", 0, [=](V& vm, const uint8_t* t, ScriptValue&) { cmd(unitRef(vm, argI(t, 8)), "canTarget", float(argI(t, 4))); });
    V::bind("CanTarget", 1, [=](V& vm, const uint8_t* t, ScriptValue&) { cmd(unitName(vm, t, 8), "canTarget", float(argI(t, 4))); });
    V::bind("Cantarget", 1, [=](V& vm, const uint8_t* t, ScriptValue&) { cmd(unitName(vm, t, 8), "canTarget", float(argI(t, 4))); });
    V::bind("AutoScan", 0, [=](V& vm, const uint8_t* t, ScriptValue&) { cmd(unitRef(vm, argI(t, 8)), "autoScan", float(argI(t, 4))); });
    V::bind("AutoScan", 1, [=](V& vm, const uint8_t* t, ScriptValue&) { cmd(unitName(vm, t, 8), "autoScan", float(argI(t, 4))); });
    V::bind("AutoAim", 0, [=](V& vm, const uint8_t* t, ScriptValue&) { cmd(unitRef(vm, argI(t, 8)), "autoAim", float(argI(t, 4))); });
    V::bind("AutoAim", 1, [=](V& vm, const uint8_t* t, ScriptValue&) { cmd(unitName(vm, t, 8), "autoAim", float(argI(t, 4))); });
    V::bind("SetMovementMode", 0, [=](V& vm, const uint8_t* t, ScriptValue&) { cmd(unitRef(vm, argI(t, 8)), "movementMode", float(argI(t, 4))); });
    V::bind("SetMovementMode", 1, [=](V& vm, const uint8_t* t, ScriptValue&) { cmd(unitName(vm, t, 8), "movementMode", float(argI(t, 4))); });
    V::bind("SetStandingMode", 0, [=](V& vm, const uint8_t* t, ScriptValue&) { cmd(unitRef(vm, argI(t, 8)), "standingMode", float(argI(t, 4))); });
    V::bind("SetStandingMode", 1, [=](V& vm, const uint8_t* t, ScriptValue&) { cmd(unitName(vm, t, 8), "standingMode", float(argI(t, 4))); });
    V::bind("Forward", 0, [=](V& vm, const uint8_t* t, ScriptValue&) { cmd(vm.owner(), "forward", float(argI(t, 4))); });
    V::bind("Backward", 0, [=](V& vm, const uint8_t* t, ScriptValue&) { cmd(vm.owner(), "backward", float(argI(t, 4))); });
    V::bind("PausePath", 0, [=](V& vm, const uint8_t* t, ScriptValue&) { cmd(unitRef(vm, argI(t, 8)), "pausePath", float(argI(t, 4))); });
    V::bind("PausePath", 1, [=](V& vm, const uint8_t* t, ScriptValue&) { cmd(unitName(vm, t, 8), "pausePath", float(argI(t, 4))); });
    V::bind("SetCombatTactic", 0, [=](V& vm, const uint8_t* t, ScriptValue&) { cmd(vm.owner(), "combatTactic", 0, 0, 0, 0, vm.strings().get(argI(t, 4))); });
    V::bind("SetNonCombatTactic", 0, [=](V& vm, const uint8_t* t, ScriptValue&) { cmd(vm.owner(), "nonCombatTactic", 0, 0, 0, 0, vm.strings().get(argI(t, 4))); });
    V::bind("SetGeneralTactic", 0, [=](V& vm, const uint8_t* t, ScriptValue&) {
        std::string tac = vm.strings().get(argI(t, 4));
        cmd(vm.owner(), "combatTactic", 0, 0, 0, 0, tac), cmd(vm.owner(), "nonCombatTactic", 0, 0, 0, 0, tac);
    });
    V::bind("LookAt", 0, [=](V& vm, const uint8_t* t, ScriptValue&) { cmd(vm.owner(), "lookAt", float(unitName(vm, t, 4))); });
    V::bind("LookAt", 1, [=](V& vm, const uint8_t* t, ScriptValue&) { cmd(vm.owner(), "lookAt", float(unitRef(vm, argI(t, 4)))); });
    V::bind("InstantDeath", 0, [=](V& vm, const uint8_t*, ScriptValue&) { cmd(vm.owner(), "instantDeath"); });
    V::bind("InstantDeath", 1, [=](V& vm, const uint8_t* t, ScriptValue&) { cmd(unitName(vm, t, 4), "instantDeath"); });
    V::bind("SetFlags", 0, [=](V& vm, const uint8_t* t, ScriptValue&) { cmd(unitRef(vm, argI(t, 8)), "flags", float(argI(t, 4))); });
    V::bind("SetFlags", 1, [=](V& vm, const uint8_t* t, ScriptValue&) { cmd(unitName(vm, t, 8), "flags", float(argI(t, 4))); });
    V::bind("ActivateFreeRange", 0, [=](V& vm, const uint8_t* t, ScriptValue&) { cmd(unitRef(vm, argI(t, 0xc)), "freeRange", argF(t, 8), float(argI(t, 4))); });
    // Teleport: (unit, position) and (unit, position, look-at point); unit by name or reference.
    V::bind("Teleport", 0, [=](V& vm, const uint8_t* t, ScriptValue&) { cmd(unitName(vm, t, 0x10), "teleport", argF(t, 0xc), argF(t, 8), argF(t, 4)); });
    V::bind("Teleport", 1, [=](V& vm, const uint8_t* t, ScriptValue&) { cmd(unitRef(vm, argI(t, 0x10)), "teleport", argF(t, 0xc), argF(t, 8), argF(t, 4)); });
    V::bind("Teleport", 2, [=](V& vm, const uint8_t* t, ScriptValue&) {
        int u = unitName(vm, t, 0x1c);
        cmd(u, "teleport", argF(t, 0x18), argF(t, 0x14), argF(t, 0x10));
        cmd(u, "face", argF(t, 0xc), argF(t, 8), argF(t, 4));
    });
    V::bind("Teleport", 3, [=](V& vm, const uint8_t* t, ScriptValue&) {
        int u = unitRef(vm, argI(t, 0x1c));
        cmd(u, "teleport", argF(t, 0x18), argF(t, 0x14), argF(t, 0x10));
        cmd(u, "face", argF(t, 0xc), argF(t, 8), argF(t, 4));
    });
    V::bind("SetWaterLevel", 0, [](V&, const uint8_t* t, ScriptValue&) { g_sys->world().unitCommand(-1, "water", argF(t, 4)); });
    V::bind("SetTeamDefcon", 0, [](V&, const uint8_t* t, ScriptValue&) { g_sys->world().teamDefcon(argI(t, 8), argI(t, 4), 0); });
    V::bind("SetMinDefcon", 0, [](V&, const uint8_t* t, ScriptValue&) { g_sys->world().teamDefcon(argI(t, 8), argI(t, 4), 1); });
    V::bind("SetMaxDefcon", 0, [](V&, const uint8_t* t, ScriptValue&) { g_sys->world().teamDefcon(argI(t, 8), argI(t, 4), 2); });
    V::bind("GetTeamDefcon", 0, [](V&, const uint8_t* t, ScriptValue& r) { r.setInt(g_sys->world().getTeamDefcon(argI(t, 4))); });
    // BeginFade (FUN_10093a70): a non-zero first argument fades in from black, zero fades out to black.
    V::bind("BeginFade", 0, [](V& vm, const uint8_t* t, ScriptValue&) {
        g_sys->fade = ScriptSystem::Fade{true, argI(t, 8) != 0, std::max(0.5f, argF(t, 4)), 0, -1, &vm};
    });
    V::bind("BeginFade", 1, [](V& vm, const uint8_t* t, ScriptValue&) {
        int sig = argI(t, 4);
        g_sys->fade = ScriptSystem::Fade{true, argI(t, 0xc) != 0, std::max(0.5f, argF(t, 8)), 0, sig >= 0 && sig < 64 ? sig : -1, &vm};
    });
    // effects the port doesn't model yet: accept silently
    V::bind("ActivateFountain", 0, [](V& vm, const uint8_t* t, ScriptValue&) { g_sys->world().activateFountain(vm.strings().get(argI(t, 8)), argI(t, 4) != 0); });
    V::bind("ActivateLight", 0, [](V& vm, const uint8_t* t, ScriptValue&) { g_sys->world().activateLight(vm.strings().get(argI(t, 8)), argI(t, 4) != 0); });
    V::bind("SetWeatherType", 0, [](V&, const uint8_t* t, ScriptValue&) { g_sys->world().weather(argI(t, 4), -1); });
    V::bind("SetWeather", 0, [](V&, const uint8_t* t, ScriptValue&) { g_sys->world().weather(-1, argI(t, 4)); });
    // Accepted without effect in the port:
    //  SetGlobalWindVariables (FUN_100d3980) wind for particles; SetSmoking (FUN_100d3a40) a damaged unit's smoke;
    //  ActivateTacticalAccess (FUN_100d4070) enables the tactical map view (+0x611), which the port doesn't have;
    //  SetMoveAvoid (FUN_100d3f90) unit flag 0x10000 that nothing in the sim reads; SaveMission (FUN_100d4380) calls
    //  FUN_1008db90, which only returns 1; Defend (FUN_100d8b10) is an empty function in the original.
    for (const char* n : {"SetGlobalWindVariables", "ActivateTacticalAccess", "SetSmoking", "SetTacticalClass", "SetTactic",
                          "BestTactic", "SquadCombatTactic", "SquadNonCombatTactic", "ChangeWeapon", "SetMoveAvoid",
                          "SaveMission", "Defend"})
        for (int ov = 0; ov < 3; ov++) V::bind(n, ov, [](V&, const uint8_t*, ScriptValue&) {});
    // AutoIgnore (FUN_100d7310) / AutoAbort (FUN_100d7370): event-slot flags of the running script.
    V::bind("AutoIgnore", 0, [](V& vm, const uint8_t* t, ScriptValue&) { vm.eventFlags(argI(t, 4), 0, 0x18); });
    V::bind("AutoAbort", 0, [](V& vm, const uint8_t* t, ScriptValue&) { vm.eventFlags(argI(t, 4), 0x10, 0); });
    V::bind("AllowCinematicCombat", 0, [](V&, const uint8_t* t, ScriptValue&) { g_sys->allowCinematicCombat = argI(t, 4) != 0; });
    // Persistent (FUN_100d3d40): the script keeps running after its unit is destroyed.
    V::bind("Persistent", 0, [](V& vm, const uint8_t* t, ScriptValue&) { vm.persistent = argI(t, 4) != 0; });
    // Kill (FUN_100d3e20 / FUN_100d3e50): vtable +200, the unit's ordinary destruction.
    V::bind("Kill", 0, [](V& vm, const uint8_t* t, ScriptValue&) { g_sys->world().destroyUnit(unitRef(vm, argI(t, 4))); });
    V::bind("Kill", 1, [](V& vm, const uint8_t* t, ScriptValue&) { g_sys->world().destroyUnit(unitName(vm, t, 4)); });
    // FullStop (FUN_100d44f0 / FUN_100d4530): motion zeroed (+0x158(0,0)); unlike FullBrake the orders stay.
    V::bind("FullStop", 0, [=](V& vm, const uint8_t* t, ScriptValue&) { cmd(unitRef(vm, argI(t, 4)), "fullStop"); });
    V::bind("FullStop", 1, [=](V& vm, const uint8_t* t, ScriptValue&) { cmd(unitName(vm, t, 4), "fullStop"); });
    // GetUnitDistance (FUN_100d4840 / FUN_100d4920): from the script's unit to the given one; 0 if either is missing.
    auto distance = [](V& vm, int unit, ScriptValue& r) {
        float a[3], b[3];
        bool ok = vm.owner() >= 0 && unit >= 0 && g_sys->world().unitPosition(vm.owner(), a) && g_sys->world().unitPosition(unit, b);
        r.setFloat(ok ? std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])) : 0.f);
    };
    V::bind("GetUnitDistance", 0, [=](V& vm, const uint8_t* t, ScriptValue& r) { distance(vm, unitName(vm, t, 4), r); });
    V::bind("GetUnitDistance", 1, [=](V& vm, const uint8_t* t, ScriptValue& r) { distance(vm, unitRef(vm, argI(t, 4)), r); });
    // Position (FUN_100d6cf0 / FUN_100d6da0): SetMovementMode and SetStandingMode in one (both nibbles of +0x1d0).
    V::bind("Position", 0, [=](V& vm, const uint8_t* t, ScriptValue&) {
        int u = unitRef(vm, argI(t, 8));
        cmd(u, "movementMode", float(argI(t, 4))), cmd(u, "standingMode", float(argI(t, 4)));
    });
    V::bind("Position", 1, [=](V& vm, const uint8_t* t, ScriptValue&) {
        int u = unitName(vm, t, 8);
        cmd(u, "movementMode", float(argI(t, 4))), cmd(u, "standingMode", float(argI(t, 4)));
    });
    // Teleport onto another unit (FUN_100d4cd0 ref/ref, FUN_100d4d30 ref/name, FUN_100d4df0 name/ref,
    // FUN_100d4eb0 name/name): the mover (-8) takes the other unit's (-4) placement.
    auto teleportTo = [=](int mover, int onto) {
        float p[3];
        if (mover < 0 || onto < 0 || !g_sys->world().unitPosition(onto, p)) return;
        cmd(mover, "teleport", p[0], p[1], p[2]);
    };
    V::bind("Teleport", 4, [=](V& vm, const uint8_t* t, ScriptValue&) { teleportTo(unitRef(vm, argI(t, 8)), unitRef(vm, argI(t, 4))); });
    V::bind("Teleport", 5, [=](V& vm, const uint8_t* t, ScriptValue&) { teleportTo(unitRef(vm, argI(t, 8)), unitName(vm, t, 4)); });
    V::bind("Teleport", 6, [=](V& vm, const uint8_t* t, ScriptValue&) { teleportTo(unitName(vm, t, 8), unitRef(vm, argI(t, 4))); });
    V::bind("Teleport", 7, [=](V& vm, const uint8_t* t, ScriptValue&) { teleportTo(unitName(vm, t, 8), unitName(vm, t, 4)); });
    // ArtilleryStrike(position, delay, shells) (FUN_100d8b20 -> the "Artillery" object, FUN_100348b0 / FUN_100349d0).
    V::bind("ArtilleryStrike", 0, [](V&, const uint8_t* t, ScriptValue&) {
        float p[3] = {argF(t, 0x14), argF(t, 0x10), argF(t, 0xc)};
        float delay = argF(t, 8);
        int count = argI(t, 4);
        // tolerate an int delay / a float count
        if (int32_t di = argI(t, 8); di > 0 && di < 100000) delay = float(di);
        if (count > 100000 || count < 0) count = int(argF(t, 4));
        if (g_sys->verbose) SDL_Log("[script] artillery at %.0f,%.0f,%.0f in %.1fs, %d shells", p[0], p[1], p[2], delay, count);
        g_sys->world().artilleryStrike(p, std::max(0.f, delay), std::max(0, std::min(count, 200)));
    });
    V::bind("FlushDialog", 0, [](V&, const uint8_t*, ScriptValue&) { g_sys->world().flushDialog(); });
    // Objectives (FUN_10133a40 / FUN_10133ad0): 8 slots of text + status.
    auto setObjective = [](int slot, const std::string* text, int status) {
        if (slot < 0 || slot >= 8) return;
        if (text) g_sys->objectives[slot].text = *text;
        g_sys->objectives[slot].status = status;
        if (g_sys->verbose) SDL_Log("[script] objective %d = %d %s", slot, status, g_sys->objectives[slot].text.c_str());
    };
    V::bind("SetUserObjective", 0, [=](V&, const uint8_t* t, ScriptValue&) { setObjective(argI(t, 8), nullptr, argI(t, 4)); });
    V::bind("SetUserObjective", 1, [=](V& vm, const uint8_t* t, ScriptValue&) {
        std::string s = vm.strings().get(argI(t, 8));
        setObjective(argI(t, 12), &s, argI(t, 4));
    });
    V::bind("SetUserObjective", 2, [=](V&, const uint8_t* t, ScriptValue&) {
        std::string s = g_sys->message(argI(t, 8));
        if (!s.empty()) setObjective(argI(t, 12), &s, argI(t, 4));
    });
    // Nav checkpoints (FUN_10108320): 26 slots with a position and a label ("Check %d" if none).
    auto setCheckpoint = [](int id, const uint8_t* t, int unit, const std::string& label) {
        if (id < 0 || id >= 26) return;
        auto& c = g_sys->checkpoints[id];
        c.active = true, c.unit = unit;
        c.pos[0] = argF(t, 0x18), c.pos[1] = argF(t, 0x14), c.pos[2] = argF(t, 0x10);
        c.label = label.empty() ? "CHECK " + std::to_string(id) : label;
        g_sys->selectedCheckpoint = id;
        if (g_sys->verbose) SDL_Log("[script] checkpoint %d %s at %.0f,%.0f", id, c.label.c_str(), c.pos[0], c.pos[2]);
    };
    V::bind("SetUserCheckpoint", 0, [=](V& vm, const uint8_t* t, ScriptValue&) { setCheckpoint(argI(t, 0x1c), t, -1, vm.strings().get(argI(t, 4))); });
    V::bind("SetUserCheckpoint", 1, [=](V& vm, const uint8_t* t, ScriptValue&) {
        setCheckpoint(argI(t, 0x20), t, unitName(vm, t, 0x1c), vm.strings().get(argI(t, 4)));
    });
    V::bind("SetUserCheckpoint", 2, [=](V& vm, const uint8_t* t, ScriptValue&) {
        setCheckpoint(argI(t, 0x20), t, unitRef(vm, argI(t, 0x1c)), vm.strings().get(argI(t, 4)));
    });
    V::bind("SetUserCheckpoint", 3, [=](V&, const uint8_t* t, ScriptValue&) { setCheckpoint(argI(t, 0x1c), t, -1, g_sys->message(argI(t, 4))); });
    V::bind("SetUserCheckpoint", 4, [=](V& vm, const uint8_t* t, ScriptValue&) {
        setCheckpoint(argI(t, 0x20), t, unitName(vm, t, 0x1c), g_sys->message(argI(t, 4)));
    });
    V::bind("SetUserCheckpoint", 5, [=](V& vm, const uint8_t* t, ScriptValue&) {
        setCheckpoint(argI(t, 0x20), t, unitRef(vm, argI(t, 0x1c)), g_sys->message(argI(t, 4)));
    });
    V::bind("SelectCheckpoint", 0, [](V&, const uint8_t* t, ScriptValue&) { g_sys->selectCheckpoint(argI(t, 4)); });
    V::bind("NextCheckpoint", 0, [](V&, const uint8_t*, ScriptValue&) { g_sys->stepCheckpoint(1); });
    V::bind("PrevCheckpoint", 0, [](V&, const uint8_t*, ScriptValue&) { g_sys->stepCheckpoint(-1); });
    V::bind("KillCheckpoint", 0, [](V&, const uint8_t* t, ScriptValue&) {
        int id = argI(t, 4);
        if (id < 0 || id >= 26) return;
        g_sys->checkpoints[id].active = false;
        if (g_sys->selectedCheckpoint == id) g_sys->selectedCheckpoint = -1, g_sys->stepCheckpoint(1);
    });
    // Centre-screen text (FUN_101338c0), shown for 5 seconds.
    auto showText = [](const std::string& s) {
        if (s.empty()) return;
        g_sys->screenText = s, g_sys->screenTextUntil = g_sys->time() + 5.f;
        if (g_sys->verbose) SDL_Log("[script] text: %s", s.c_str());
    };
    V::bind("RegisterString", 0, [=](V& vm, const uint8_t* t, ScriptValue&) { showText(vm.strings().get(argI(t, 4))); });
    V::bind("RegisterString", 1, [=](V&, const uint8_t* t, ScriptValue&) { showText(g_sys->message(argI(t, 4))); });
    // Focus: the script's unit watches another unit and gets event 23 when it comes within range.
    V::bind("SetFocusNotifyRange", 0, [](V& vm, const uint8_t* t, ScriptValue&) { g_sys->focus_[&vm].range = argF(t, 4); });
    V::bind("SetUnitFocus", 0, [](V& vm, const uint8_t* t, ScriptValue& r) {
        int u = unitName(vm, t, 4);
        bool ok = u >= 0 && g_sys->world().isAlive(u);
        if (ok) g_sys->focus_[&vm].target = u, g_sys->focus_[&vm].inside = false;
        r.setInt(ok);
    });
    V::bind("SetUnitFocus", 1, [](V& vm, const uint8_t* t, ScriptValue& r) {
        int u = unitRef(vm, argI(t, 4));
        bool ok = u >= 0 && g_sys->world().isAlive(u);
        if (ok) g_sys->focus_[&vm].target = u, g_sys->focus_[&vm].inside = false;
        r.setInt(ok);
    });
    // GetUnitStatus returns a UNIT_INFO struct (6 ints, FUN_100d6850).
    auto unitInfo = [](V& vm, int unit, ScriptValue& r) {
        int32_t info[6] = {0, 0, 0, 0, 0, 0};
        if (unit >= 0) g_sys->world().unitStatus(unit, info);
        r.type = vm.typeId("UNIT_INFO");
        if (!r.type) r.type = 1;
        r.big.assign(reinterpret_cast<uint8_t*>(info), reinterpret_cast<uint8_t*>(info) + sizeof info);
    };
    V::bind("GetUnitStatus", 0, [=](V& vm, const uint8_t* t, ScriptValue& r) { unitInfo(vm, unitRef(vm, argI(t, 4)), r); });
    V::bind("GetUnitStatus", 1, [=](V& vm, const uint8_t* t, ScriptValue& r) { unitInfo(vm, unitName(vm, t, 4), r); });
    V::bind("LockForwardStick", 0, [](V& vm, const uint8_t* t, ScriptValue&) { g_sys->world().lockForwardStick(unitRef(vm, argI(t, 8)), argI(t, 4) != 0); });
    V::bind("LockForwardStick", 1, [](V& vm, const uint8_t* t, ScriptValue&) { g_sys->world().lockForwardStick(unitName(vm, t, 8), argI(t, 4) != 0); });
    V::bind("SetHitpoints", 0, [](V& vm, const uint8_t* t, ScriptValue&) { g_sys->world().setHitpoints(unitRef(vm, argI(t, 8)), argI(t, 4)); });
    V::bind("SetHitpoints", 1, [](V& vm, const uint8_t* t, ScriptValue&) { g_sys->world().setHitpoints(unitName(vm, t, 8), argI(t, 4)); });
    V::bind("SetInvincible", 0, [](V& vm, const uint8_t* t, ScriptValue&) { g_sys->world().setInvincible(unitRef(vm, argI(t, 8)), argI(t, 4) != 0); });
    V::bind("Invincible", 0, [](V& vm, const uint8_t* t, ScriptValue&) { g_sys->world().setInvincible(unitRef(vm, argI(t, 8)), argI(t, 4) != 0); });
    V::bind("Invincible", 1, [](V& vm, const uint8_t* t, ScriptValue&) { g_sys->world().setInvincible(unitName(vm, t, 8), argI(t, 4) != 0); });
    V::bind("SetInvincible", 1, [](V& vm, const uint8_t* t, ScriptValue&) { g_sys->world().setInvincible(unitName(vm, t, 8), argI(t, 4) != 0); });
    V::bind("TriggerSound", 0, [](V& vm, const uint8_t* t, ScriptValue&) { g_sys->world().playSound(vm.strings().get(argI(t, 4)), false); });
    V::bind("AutoBreak", 0, [](V& vm, const uint8_t* t, ScriptValue&) { vm.markBreak(argI(t, 4)); });
    V::bind("GetUnitPosition", 0, [](V& vm, const uint8_t* t, ScriptValue& r) {
        float p[3] = {0, 0, 0};
        g_sys->world().unitPosition(g_sys->world().findUnit(vm.strings().get(argI(t, 4))), p);
        r.setVec(p[0], p[1], p[2]);
    });
    V::bind("GetUnitPosition", 1, [](V& vm, const uint8_t* t, ScriptValue& r) {
        float p[3] = {0, 0, 0};
        g_sys->world().unitPosition(unitRef(vm, argI(t, 4)), p);
        r.setVec(p[0], p[1], p[2]);
    });
}

}  // namespace hg2
