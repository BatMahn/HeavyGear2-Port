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
    // space only (+0x1040, FUN_10063a10): Increase / Decrease Throttle give +1 / -1 (0 with neither), plus the
    // throttle setting in forward mode or minus it in reverse; the thrusters use it off the ground (FUN_1003c480)
    float thrust = 0;
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
    // Crippled (ECY trigger 15, sent by FUN_10062430 when a critical takes Movement to 1): the state machine moves to
    // the crippled group (23 .. 36 on the gears: no run, jump or skates) and stays there.
    void cripple() { crippleWant_ = true; }
    bool crippled() const { return crippled_; }
    // Downward speed (m/s) of the last ground contact since the previous call (FUN_1003cc30's hard-landing test).
    float takeImpact() { float v = impact_; impact_ = 0; return v; }
    // Units this body's parts were pushed into since the last call (FUN_100a63d0's part loop between two dynamic
    // proxies: the pair's ramming damage, Combat::ram).
    std::vector<int> takeRams() { std::vector<int> r; r.swap(rams_); return r; }
    float pitch() const { return pitch_; }  // body tilt (radians; + nose up / + right side up), vehicles only
    float roll() const { return roll_; }
    float twist = 0;                  // torso twist (radians) applied to frm-tst
    bool forwardLocked() const { return lockForward_; }
    float eyeHeight() const;
    // External camera (FUN_1007c9f0): pivots on the body's bounding-sphere centre, `zoom` sphere radii away, raised
    // by `pitch` and turned by `orbit` about the body's own heading (it follows the root, so a sidestep swings it).
    void chaseCamera(Vec3& eye, Vec3& target, float pitch, float orbit, float zoom = 2.f) const;
    float boundingRadius() const { return boundR_; }
    float legAngle() const { return legs_; }  // sidestep leg swing (radians, + right)
    // Cockpit camera at the pilot's eye looking along yaw + orbit.
    void cockpitCamera(Vec3& eye, Vec3& target, float pitch, float orbit) const;

    Audio* audio = nullptr;    // footsteps / engine when set
    Particles* particles = nullptr;  // foot dust when set
    float soundVolume = 1.f;
    bool engineLoop = false;  // looping motor sound (player only)
    float speedScale = 1.f;   // movement criticals: current / original Movement (+0xbf8, vmax follows it)
    int maneuverNow = 0x7fff; // Maneuverability after criticals (+0xbf4, FUN_1003aaf0); 0x7fff: the definition's
    int jumpJets = 0;         // Jump Jets perk level (+0xc30 bits 24-25): the Jump Jets key needs it
    // Space missions (DAT_1026667c + 0x14d): the gear runs FUN_1003b850 instead of FUN_1003af10 (no gravity; a 1 g
    // pull toward the surface it touches; thrusters while airborne).
    bool space = false;
    bool pilot = false;       // the local player's gear (+0x1f0): slope forces, the 0.5 m ground contact

private:
    bool fire(int trigger);    // take the ECY transition for `trigger` from the current state
    void resolveStates(int base = 0, bool reset = true);  // well-known states, followed from state `base`
    int stateOf(int from, int trigger) const;
    void sounds();
    void footfall(float side = 0);
    // Ground drive, resistance, traction and slope forces (FUN_1003af10's force terms) on the local velocity.
    void drive(float dt, float throttle, float targetStrafe, bool turning, bool ecy, const Vec3& fwd, const Vec3& right);
    float vmax() const;        // +0x270 for the current state
    bool movingState() const;  // +0x124a: a state whose resistance is not x10
    // Play the current locomotion cycle for dt at body speed v (signed); returns the model-space root travel. Takes a
    // pending walk <-> run change (wantCycle_) at the transition's sync points.
    Vec3 advanceCycle(float dt, float v);
    float cycleStride(int set);  // a cycle's root travel per loop (m), cached
    // Collision (see "Collision" in docs/FORMATS.md). The support under the body from the three downward probes
    // (FUN_1003d640 segments 0-2, FUN_1003db20); contacts from the forward probe (segment 3) and from the body's parts
    // pushed through objects (FUN_100a63d0's second loop), gathered after the move and applied to the velocity in the
    // next physics step (FUN_10058710).
    struct Contact { Vec3 p, n; float depth, k, c; };
    float support(Vec3& normal);
    // Per-class ground contact (the class vtables' +0x28c probes and +0x170 contact handlers; see "Collision" in
    // docs/FORMATS.md): bipeds, infantry, chicken walkers and frames use the biped probes above; Jeeps (0x4e) ride
    // on wheel springs, Mounts (0x51) on four leg springs, hovertanks (0x4f) on a height-limited lift. Those three
    // tilt with the ground (pitch / roll) and run their own vertical step (chassisStep) instead of the biped's.
    enum Chassis { kBiped, kWheels, kHover, kLegs };
    struct Wheel {
        Vec3 p;               // body-frame point (record +0x100)
        float len = 1;        // suspension length L (+0x118)
        float k = 0, c = 0;   // spring (+0x130) and damping (+0x138), per unit mass
        float comp = 0, prev = 0;  // compression 1 - d / L (+0x120) and last step's (+0x128)
        Vec3 n{0, 1, 0};      // the ground's normal under it (+0x140)
        float ground = 0;     // the ground's height under it (+0x168)
        bool touch = false;   // d <= L (+0x178)
        int fb = 0;           // the pitch torque's sign by record: +1 a front wheel / leg, -1 a back one, 0 middle
    };
    void setupChassis(int type);
    void chassisStep(float dt);
    Mat4 bodyRotation() const;  // rotZ(roll) rotX(pitch) rotY(yaw)
    int chassis_ = kBiped;
    std::vector<Wheel> wheels_;
    float pitch_ = 0, roll_ = 0, pitchRate_ = 0, rollRate_ = 0;
    float tiltK_ = 0, tiltD_ = 0, tiltDz_ = 0;  // +0x1000 and the angular damping +0x1008 / +0x1018
    float massK_ = 100.f;                       // mass = massK x r^3 (+0xd98)
    float probeUp_ = 0, probeDown_ = 0;         // the wheel / leg probe segment above and below each point
    std::vector<int> rams_;
    void gatherContacts();
    void applyContacts(float dt, const Vec3& fwd, const Vec3& right);
    int unitFilter() const;
    float radius();            // object +0x98

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
    std::vector<Contact> contacts_;
    Vec3 supportN_{0, 1, 0};   // +0xfc0: the normal of what the body stands on
    float radius_ = -1;
    bool crippled_ = false, crippleWant_ = false;
    float impact_ = 0;
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
    // sidestep (FUN_1003a9c0): the commanded leg angle (+0x10e8, 300 deg/s to +-90 deg) and its filtered value
    // (+0x10d8, filter at +0x10c0: y = 0.8 y + 0.2 x) with last step's (+0x10e0); radians, + = the root turned right
    float legTarget_ = 0, legs_ = 0, legsPrev_ = 0;
    // foot falls (FUN_10067570): in the window (model +0x168), past it (+0x169), stepped last frame (+0x16a)
    bool stepIn_ = false, stepPast_ = false, stepPrev_ = false;
    // bounding sphere (unit +0x80 centre in the body's frame, +0x98 radius), taken from the first posed frame
    Vec3 boundC_;
    float boundR_ = 0;
};

}  // namespace hg2
