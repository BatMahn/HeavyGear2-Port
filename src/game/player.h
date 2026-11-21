// Gear body: driving, posture (stand / kneel / prone), jumping, skates, the .ECY animation state
// machine, ground following and the chase / cockpit cameras. Used for the player and AI gears.
#pragma once
#include "../assets/gear.h"
#include "../core/math.h"
#include "scene.h"

namespace hg2 {

class Audio;
class Particles;

struct PlayerInput {
    // Throttle command (+0xf48 through FUN_1003aaf0): -0.5 .. 1, drive force = drive constant x throttle. The pilot's
    // value comes from the Forward / Backward axis and the throttle setting (FUN_10063a10, see Session).
    float throttle = 0;
    bool run = false;    // AI / remote bodies only: without it the throttle is held to a walking pace
    float turn = 0;      // -1 .. 1 (left / right)
    float strafe = 0;    // -1 .. 1 sidestep
    // edge-triggered: Crouch (ECY 6), Stand Up (7), Jump (8), Jump Jets (9), Wheels (skates, 4)
    bool down = false, up = false, jump = false, jumpJets = false, toggleSkates = false;
};

class Player {
public:
    bool init(const FileSystem& fs, Scene& scene);                 // the mission's user gear
    bool initObject(const FileSystem& fs, Scene& scene, int object);  // any gear / vehicle object
    bool canWalk() const { return !ecy_.states.empty(); }
    void setYaw(float y) { yaw_ = y; }
    void teleport(const Vec3& p) { pos_ = p, vy_ = 0, air_ = false; }
    void halt() { speed_ = 0, strafe_ = 0; }  // FullStop: motion zeroed on the spot
    void update(float dt, const PlayerInput& in);
    bool active() const { return object_ >= 0; }
    int object() const { return object_; }
    Vec3 position() const { return pos_; }
    float yaw() const { return yaw_; }
    float speed() const { return speed_; }
    const GearDef& def() const { return def_; }
    // Script posture code (GetUnitStatus): 0 prone, 1 standing, 2 kneeling, 3 skating (SMS).
    int postureCode() const;
    bool skating() const { return state_ >= 0 && (state_ == sSkate_ || state_ == sSkateOn_); }
    bool airborne() const { return air_; }
    void lockForward(bool on) { lockForward_ = on; }
    // Orders for AI bodies: posture to settle into (0 prone, 1 stand, 2 kneel; -1 none) and skates.
    void setPosture(int code) { wantPosture_ = code; }
    void setSkates(bool on) { if (canWalk() && on != skating() && def_.movement2 > 0) skateReq_ = true; }
    // Hit reaction (ECY triggers 12 / 13 / 14 for light / medium / heavy damage, kind-3 transitions to a body
    // animation played over the current one).
    void hitReaction(int level);
    void revive();                    // stand up again (network respawn)
    void setInputState(bool airborne, bool skates) { if (skates != skating()) setSkates(skates); (void)airborne; }
    // Knock-down (criticals 0x80000 and heavy hits, ECY triggers 0x11 .. 0x14): the gear falls and stays down
    // until a Stand Up (trigger 7).
    void knockDown(int trigger);
    bool knockedDown() const;
    void die();                       // fall animation (ECY triggers 16..20); the body then lies still
    bool dead() const { return dead_; }
    float twist = 0;                  // torso twist (radians) applied to frm-tst
    bool forwardLocked() const { return lockForward_; }
    float eyeHeight() const;
    // Chase camera behind and above the gear; `dist` is the zoom.
    void chaseCamera(Vec3& eye, Vec3& target, float pitch, float orbit, float dist = 16.f) const;
    // Cockpit camera at the pilot's eye looking along yaw + orbit.
    void cockpitCamera(Vec3& eye, Vec3& target, float pitch, float orbit) const;

    Audio* audio = nullptr;    // footsteps / engine when set
    Particles* particles = nullptr;  // foot dust when set
    float soundVolume = 1.f;
    bool engineLoop = false;  // looping motor sound (player only)
    float speedScale = 1.f;   // movement criticals: current / original Movement (+0xbf8, vmax follows it)
    int maneuverNow = 0x7fff; // Maneuverability after criticals (+0xbf4, FUN_1003aaf0); 0x7fff: the definition's
    int jumpJets = 0;         // Jump Jets perk level (+0xc30 bits 24-25): the Jump Jets key needs it
    float gravityScale = 1.f; // space missions: low gravity
    bool pilot = false;       // the local player's gear (+0x1f0): slope forces, the 0.5 m ground contact

private:
    bool fire(int trigger);    // take the ECY transition for `trigger` from the current state
    void resolveStates();
    int stateOf(int from, int trigger) const;
    void sounds(float prevPhase);
    void footfall();
    // Ground drive, resistance, traction and slope forces (FUN_1003af10's force terms) on the local velocity.
    void drive(float dt, float throttle, float targetStrafe, bool turning, bool ecy, const Vec3& fwd, const Vec3& right);
    float vmax() const;        // +0x270 for the current state
    bool movingState() const;  // +0x124a: a state whose resistance is not x10
    // Play the current locomotion cycle for dt at body speed v (signed); returns the model-space root travel. Takes a
    // pending walk <-> run change (wantCycle_) at the transition's sync points.
    Vec3 advanceCycle(float dt, float v);
    float cycleStride(int set);  // a cycle's root travel per loop (m), cached

    Scene* scene_ = nullptr;
    int object_ = -1;
    GearDef def_;
    EcyScript ecy_;
    Vec3 pos_;
    float yaw_ = 0, speed_ = 0, strafe_ = 0, vy_ = 0, frame_ = 0;
    float yawRate_ = 0;        // +0xf28
    float thrust_ = 0;         // +0x1070: jump thrust time left
    float rate_ = 30.f;        // playback rate of the current one-shot / idle set (the ECY transition's rate)
    float vmaxWalk_ = 1, vmaxSkate_ = 1, walkLimit_ = 1;  // Movement x 6 km/h, Movement2 x 6 km/h, +0x10b4
    bool air_ = false, lockForward_ = false, skateReq_ = false;
    int wantCycle_ = -1;       // walk / run trigger waiting for a sync point
    std::vector<std::pair<int, float>> strides_;  // cycleStride cache
    int wantPosture_ = -1;
    bool dead_ = false;
    int reactSet_ = -1;
    float reactFrame_ = 0;
    std::string reactRoot_;
    int idleSet_ = 0;
    int state_ = 0, set_ = 0;
    bool loop_ = true;
    // well-known states (resolved from the ECY by following triggers from the idle state)
    int sIdle_ = 0, sWalk_ = 1, sRun_ = 2, sKneel_ = -1, sKneelWalk_ = -1, sProne_ = -1, sCrawl_ = -1;
    int sJump_ = -1, sJumpAir_ = -1, sJumpLand_ = -1;     // Jump (trigger 8)
    int sJet_ = -1, sJetAir_ = -1, sJetLand_ = -1;        // Jump Jets (trigger 9)
    int sSkateOn_ = -1, sSkate_ = -1, sSkateOff_ = -1;    // Wheels (trigger 4)
    int motor_ = -1;  // engine loop handle
};

}  // namespace hg2
