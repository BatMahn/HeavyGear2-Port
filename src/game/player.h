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
    float throttle = 0;  // -1 .. 1 (forward / back)
    bool run = false;
    float turn = 0;      // -1 .. 1 (left / right)
    float strafe = 0;    // -1 .. 1 sidestep
    bool down = false, up = false, jump = false, toggleSkates = false;  // edge-triggered
};

class Player {
public:
    bool init(const FileSystem& fs, Scene& scene);                 // the mission's user gear
    bool initObject(const FileSystem& fs, Scene& scene, int object);  // any gear / vehicle object
    bool canWalk() const { return !ecy_.states.empty(); }
    void setYaw(float y) { yaw_ = y; }
    void teleport(const Vec3& p) { pos_ = p, vy_ = 0, air_ = false; }
    void update(float dt, const PlayerInput& in);
    bool active() const { return object_ >= 0; }
    int object() const { return object_; }
    Vec3 position() const { return pos_; }
    float yaw() const { return yaw_; }
    float speed() const { return speed_; }
    const GearDef& def() const { return def_; }
    // Script posture code (GetUnitStatus): 0 prone, 1 standing, 2 kneeling, 3 skating (SMS).
    int postureCode() const;
    bool skating() const { return sms_; }
    bool airborne() const { return air_; }
    void lockForward(bool on) { lockForward_ = on; }
    // Orders for AI bodies: posture to settle into (0 prone, 1 stand, 2 kneel; -1 none) and skates.
    void setPosture(int code) { wantPosture_ = code; }
    void setSkates(bool on) { if (canWalk()) sms_ = on; }
    // Hit reaction (ECY triggers 12 / 13 / 14 for light / medium / heavy damage, kind-3 transitions to a body
    // animation played over the current one).
    void hitReaction(int level);
    void revive();                    // stand up again (network respawn)
    void setInputState(bool airborne, bool skates) { sms_ = skates && canWalk(); (void)airborne; }
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
    float speedScale = 1.f;   // leg damage / drive criticals
    float turnScale = 1.f;    // gyro (manoeuvre) criticals
    int jumpJets = 0;         // Jump Jets perk level: higher jumps
    float gravityScale = 1.f; // space missions: low gravity

private:
    bool fire(int trigger);    // take the ECY transition for `trigger` from the current state
    void resolveStates();
    int stateOf(int from, int trigger) const;
    void sounds(float prevPhase);

    Scene* scene_ = nullptr;
    int object_ = -1;
    GearDef def_;
    EcyScript ecy_;
    Vec3 pos_;
    float footprint_ = 1.4f;  // half the distance between the feet (ground probes)
    float yaw_ = 0, speed_ = 0, strafe_ = 0, vy_ = 0, frame_ = 0;
    bool air_ = false, sms_ = false, lockForward_ = false;
    int wantPosture_ = -1;
    bool dead_ = false;
    int reactSet_ = -1;
    float reactFrame_ = 0;
    std::string reactRoot_;
    int idleSet_ = 0;  // set shown while idle (walk set while turning on the spot)
    int state_ = 0, set_ = 0;
    bool loop_ = true;
    // well-known states (resolved from the ECY by following triggers from the idle state)
    int sIdle_ = 0, sWalk_ = 1, sRun_ = 2, sKneel_ = -1, sKneelWalk_ = -1, sProne_ = -1, sCrawl_ = -1;
    int sJump_ = -1, sAir_ = -1, sLand_ = -1;
    int motor_ = -1;  // engine loop handle
};

}  // namespace hg2
