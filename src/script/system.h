// Mission script system: loads the mission's script project, attaches scripts to the mission and to
// units, routes signals and timers, and binds the engine builtins.
#pragma once
#include <map>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "../assets/mission.h"
#include "vm.h"

namespace hg2 {

// Game-side interface the builtins call. The default implementation only logs; the game
// overrides what it supports as the simulation is ported.
class ScriptWorld {
public:
    virtual ~ScriptWorld() = default;
    virtual int findUnit(const std::string& name);  // -1 if none
    virtual int teamOf(int unit);
    virtual bool isAlive(int unit) { return unit >= 0; }
    virtual bool userAlive() { return true; }
    // MissionOver(code[, delay]): 1 success, 0 failure, 2 alternate outcome (MSN's third mission id).
    virtual void missionOver(int result, float delay) { (void)result, (void)delay; }
    virtual void objectiveStatus(int objective, int status) { (void)objective, (void)status; }
    virtual void playSound(const std::string& wav, bool global) { (void)wav, (void)global; }
    virtual bool unitPosition(int unit, float out[3]) { (void)unit, (void)out; return false; }
    // Unit orders (FollowPath, AutoSleep, ActivateUnit, SetSpeed, Attack, FullBrake, AutoPilot...).
    virtual int playerUnit() { return -1; }
    virtual void unitSleep(int, bool) {}
    virtual void unitActivate(int, bool) {}
    virtual void unitGoto(int, const float[3]) {}
    virtual bool unitFollowPath(int, const std::string&) { return false; }
    virtual bool unitAssignPath(int, const std::string&) { return false; }
    virtual void unitStop(int) {}
    virtual void unitSpeed(int, float) {}
    virtual void unitAttack(int, int) {}
    virtual void unitNotifyRange(int, float) {}
    virtual void unitAutoPilot(int) {}
    virtual void userControl(bool) {}
    virtual void queueDialog(const std::string&) {}
    virtual void flushDialog() {}
    // GetUnitStatus (UNIT_INFO): [0] external view, [1] posture (0 prone, 1 stand, 2 kneel, 3 skates),
    // [2] flag, [3] targeted by the user, [4] targeting someone, [5] state byte.
    virtual void unitStatus(int unit, int32_t out[6]) { (void)unit, (void)out; }
    virtual void lockForwardStick(int unit, bool on) { (void)unit, (void)on; }
    virtual void setHitpoints(int unit, int hp) { (void)unit, (void)hp; }
    virtual void setInvincible(int unit, bool on) { (void)unit, (void)on; }
    // Unit relations for script references 1 (enemy in notify range), 2 (friend in range), 3 (focus),
    // 5 (attacker / target), 7..0x17 (the user's squad mates). -1 if none.
    virtual int unitRelation(int unit, int ref) { (void)unit, (void)ref; return -1; }
    // Brain settings and orders without a dedicated hook ("visibility", "friendRange", "canTarget", "autoScan",
    // "movementMode", "standingMode", "forward", "backward", "pausePath", "combatTactic", "nonCombatTactic",
    // "lookAt", "instantDeath", "teleport", "water"…). Numbers in a/b/c/d, text in s.
    virtual void unitCommand(int unit, const std::string& cmd, float a = 0, float b = 0, float c = 0, float d = 0,
                             const std::string& s = std::string()) {
        (void)unit, (void)cmd, (void)a, (void)b, (void)c, (void)d, (void)s;
    }
    // Team alert level 0..5 (FUN_100bbcf0 / FUN_100bbe00 / FUN_100bbea0); 6 for an unknown team.
    virtual void teamDefcon(int team, int level, int mode) { (void)team, (void)level, (void)mode; }  // mode 0 set, 1 min, 2 max
    virtual int getTeamDefcon(int team) { (void)team; return 6; }
    virtual void activateFountain(const std::string& name, bool on) { (void)name, (void)on; }
    virtual void activateLight(const std::string& name, bool on) { (void)name, (void)on; }
    virtual void weather(int type, int preset) { (void)type, (void)preset; }  // type -1: keep; preset -1: keep
    // Kill (FUN_100d3e20): destroyed the normal way (explosion, wreck, event 5), unlike InstantDeath's removal.
    virtual void destroyUnit(int unit) { (void)unit; }
    // ArtilleryStrike (FUN_100d8b20): after `delay` seconds, `count` shells half a second apart land within 15 m of p.
    virtual void artilleryStrike(const float p[3], float delay, int count) { (void)p, (void)delay, (void)count; }

    std::vector<std::string> unitNames;  // index = unit id (mission object order)
    std::vector<int> unitTeams;
};

class ScriptSystem : public ScriptHost {
public:
    ScriptSystem(const FileSystem& fs, ScriptWorld& world);
    bool load(const Mission& m, std::string* err);
    void update(float dt);                  // timers, signals, run pending handlers
    void attachScript(const std::string& file, int unit);  // a unit created at run time (fountains); runs its start-up handler
    void unitSignal(int unit, int sig);
    void teamSignal(int team, int sig);
    void missionSignal(int sig);
    void worldSignal(int sig);
    void setTimer(ScriptVM& vm, float seconds, int sig);

    float timeStep() override { return dt_; }
    void log(const std::string& s) override;
    void unimplemented(ScriptVM& vm, const std::string& name, int overload) override;

    ScriptWorld& world() { return world_; }
    ScriptVM* current() { return current_; }
    int vmCount() const { return int(vms_.size()); }
    uint64_t instructions() const;
    std::set<std::string> calledNatives(bool unboundOnly) const;  // builtins the loaded scripts call (optionally only unbound ones)
    std::set<std::string> missing;  // unimplemented builtins seen
    bool verbose = false;
    float time() const { return time_; }

    // Mission user interface driven by the scripts (objectives, nav checkpoints, centre-screen text).
    struct Objective { std::string text; int status = 0; };  // status 1 active, 2 complete, 3 failed
    struct Checkpoint { bool active = false; float pos[3] = {0, 0, 0}; int unit = -1; std::string label; };
    Objective objectives[8];
    Checkpoint checkpoints[26];
    int selectedCheckpoint = -1;
    std::string screenText;
    float screenTextUntil = 0;
    // Cinematic camera (ControlCamera): moves along a mission path for `duration` seconds looking at a unit,
    // a point or another path, then raises `endSig` on the calling script (`skipSig` when skipped).
    struct Cinematic {
        bool active = false;
        const MissionPath* path = nullptr;
        const MissionPath* lookPath = nullptr;
        int lookUnit = -1;
        bool hasLookPoint = false;
        float lookPoint[3] = {0, 0, 0};
        float duration = 0, t = 0;
        int endSig = -1, skipSig = -1;
        ScriptVM* vm = nullptr;
    } cinematic;
    bool cinematicCamera(float eye[3], float at[3]);
    void skipCinematic();
    bool cinematicMode = false;
    // AllowCinematicCombat (FUN_100d67e0, DAT_10268824): brains keep fighting while cinematicMode is on.
    bool allowCinematicCombat = false;
    // Brains stand down during cinematic mode unless combat is allowed (FUN_100b9ae0 checks +0x521). Held only while a
    // scripted camera runs: some missions (S1J) never clear the mode themselves, and the port doesn't know yet what in
    // the original ends it when a camera path runs out.
    bool combatHold() const { return cinematicMode && cinematic.active && !allowCinematicCombat; }
    // BeginFade: screen fades in from black (in = true) or out to black over `time`, then raises `signal`.
    struct Fade { bool active = false, in = false; float time = 1, t = 0; int signal = -1; ScriptVM* vm = nullptr; } fade;
    float fadeAlpha() const;  // 0 clear .. 1 black
    int focusOf(ScriptVM* vm) const;  // SetCinematicControl(on): letterboxed, no HUD
    const Mission* mission() const { return mission_; }
    void startCinematic(ScriptVM& vm, const std::string& path, int endSig, int skipSig, float duration);
    const std::string& message(int index) const;  // the mission's UserMessageFile line
    bool checkpointPosition(int id, float out[3]);
    void selectCheckpoint(int id);
    void stepCheckpoint(int dir);

private:
    std::shared_ptr<ScriptProgram> program(const std::string& name, std::string* err);
    void bindBuiltins();

    const FileSystem& fs_;
    const Mission* mission_ = nullptr;
    ScriptWorld& world_;
    StringTable strings_;
    std::vector<std::shared_ptr<ScriptProgram>> programs_;
    struct Instance { std::unique_ptr<ScriptVM> vm; int unit; int deadTicks = 0; };  // unit -1 = mission script
    std::vector<Instance> vms_;
    struct Focus { int target = -1; float range = 0; bool inside = false; };  // SetUnitFocus / range: event 23
    std::map<ScriptVM*, Focus> focus_;
    bool fadeHold_ = false;
    std::map<int, std::string> messages_;
    struct Timer { ScriptVM* vm; float at; int sig; };
    std::vector<Timer> timers_;
    ScriptVM* current_ = nullptr;
    float time_ = 0, dt_ = 1.f / 30.f;
};

}  // namespace hg2
