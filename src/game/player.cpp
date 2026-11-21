#include "player.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

#include "../audio/audio.h"
#include "particles.h"

namespace hg2 {

namespace {
constexpr float kDeg = 3.14159265358979f / 180.f;
// ECY triggers (the gear's state machine inputs, sent by the pilot input FUN_10063a10 and the locomotion
// FUN_10064700): 1 stop, 2 walk, 3 run, 4 Wheels (skates on / off: 0 -> 7 -> 9 skating -> 8 -> 1), 6 Crouch,
// 7 Stand Up, 8 Jump (from walk / run: 12 -> 13 -> 14 -> run), 9 Jump Jets (15 -> 16 -> 17 -> run),
// 0x11 / 0x12 heavy-hit falls (19 / 20), 0x13 / 0x14 knock-downs (21 / 22).
constexpr int kTrigStop = 1, kTrigWalk = 2, kTrigRun = 3, kTrigWheels = 4, kTrigDown = 6, kTrigUp = 7,
              kTrigJump = 8, kTrigJets = 9, kCrippleTrig = 15;
constexpr float kGravity = 20.f;  // DAT_10163ae8: the gravity force (FUN_1003d1f0, vtable +0x274)
constexpr float kKmh = 0.278f;    // DAT_10143408: Movement points are 6 km/h each
}  // namespace

bool Player::init(const FileSystem& fs, Scene& scene) {
    const Mission& m = scene.mission();
    for (size_t i = 0; i < m.objects.size(); i++)
        if (m.objects[i].userHeader && initObject(fs, scene, int(i))) return true;
    return false;
}

bool Player::initObject(const FileSystem& fs, Scene& scene, int index) {
    scene_ = &scene;
    const Mission& m = scene.mission();
    if (index < 0 || size_t(index) >= m.objects.size()) return false;
    const MissionObject& o = m.objects[size_t(index)];
    if (o.defIndex < 0 || size_t(o.defIndex) >= m.defs.size()) return false;
    const MissionDef& d = m.defs[size_t(o.defIndex)];
    if (!parseGearDef(d.data, d.size, def_)) return false;
    object_ = index;
    pos_ = Vec3(float(o.pos[0]), float(o.pos[1]), float(o.pos[2]));
    yaw_ = float(o.rot[0]) * kDeg;
    Bytes b;
    if (!def_.animScript.empty() && fs.read(def_.animScript, b)) parseEcy(b, ecy_);
    // FUN_10035460: vmax (+0x270) = Movement (+0xbf8) x 6 km/h; the walk / run boundary (+0x10b4) is MaxWalkSpeed,
    // or 0.35 vmax without one. On skates (FUN_10064700 / FUN_10063a10) vmax is the second Movement (+0xbfc) x 6 km/h.
    vmaxWalk_ = std::max(1.f, float(def_.movement1 * 6) * kKmh);
    vmaxSkate_ = std::max(1.f, float(def_.movement2 * 6) * kKmh);
    walkLimit_ = def_.maxWalkSpeed != 0 ? def_.maxWalkSpeed : vmaxWalk_ * 0.35f;
    if (def_.maxWalkSpeed <= 0) def_.maxWalkSpeed = walkLimit_;
    if (def_.maxRunSpeed <= 0) def_.maxRunSpeed = vmaxWalk_ * 0.65f;  // +0x10b8: MaxRunSpeed, or 0.65 vmax
    resolveStates();
    if (o.userHeader)
        SDL_Log("player gear: %s movement %d/%d (vmax %.1f / skates %.1f) walk %.1f, %zu animation states (kneel %d prone %d jump %d skates %d)",
                def_.geometry.empty() ? "?" : def_.geometry[0].c_str(), def_.movement1, def_.movement2, vmaxWalk_, vmaxSkate_,
                walkLimit_, ecy_.states.size(), sKneel_, sProne_, sJump_, sSkate_);
    // on the ground; a mission without terrain (space: hulls and stations) keeps the placed height
    if (scene.hasTerrain()) pos_.y = scene.groundHeight(pos_.x, pos_.z);
    setupChassis(o.type);
    return true;
}

int Player::stateOf(int from, int trigger) const {
    if (from < 0 || size_t(from) >= ecy_.states.size()) return -1;
    for (auto& t : ecy_.states[size_t(from)].transitions)
        if (t.trigger == trigger && t.kind != 3 && t.target >= 0 && size_t(t.target) < ecy_.states.size()) return t.target;
    return -1;
}

void Player::resolveStates(int base, bool reset) {
    if (ecy_.states.empty()) return;
    sIdle_ = base;
    sWalk_ = stateOf(sIdle_, kTrigWalk);
    sRun_ = stateOf(sWalk_, kTrigRun);
    sKneel_ = stateOf(sIdle_, kTrigDown);
    sProne_ = stateOf(sKneel_, kTrigDown);
    sKneelWalk_ = stateOf(sKneel_, kTrigWalk);
    sCrawl_ = stateOf(sProne_, kTrigWalk);
    sJump_ = stateOf(sWalk_, kTrigJump);
    sJumpAir_ = stateOf(sJump_, kTrigJump);
    sJumpLand_ = stateOf(sJumpAir_, kTrigJump);
    sJet_ = stateOf(sIdle_, kTrigJets);
    sJetAir_ = stateOf(sJet_, kTrigJets);
    sJetLand_ = stateOf(sJetAir_, kTrigJets);
    sSkateOn_ = stateOf(sIdle_, kTrigWheels);
    sSkate_ = stateOf(sSkateOn_, kTrigWheels);
    sSkateOff_ = stateOf(sSkate_, kTrigWheels);
    if (reset) state_ = sIdle_, set_ = 0, loop_ = true, rate_ = 30.f;
}

bool Player::fire(int trigger) {
    if (size_t(state_) >= ecy_.states.size()) return false;
    for (auto& t : ecy_.states[size_t(state_)].transitions) {
        if (t.trigger != trigger || t.kind == 3 || t.target < 0 || size_t(t.target) >= ecy_.states.size()) continue;
        bool fromCycle = loop_ && (state_ == sWalk_ || state_ == sRun_);
        int len = std::max(1, loop_ ? scene_->animCycle(object_, set_) : scene_->animLength(object_, set_));
        float phase = loop_ ? frame_ / float(len) : 0.f;
        state_ = t.target;
        set_ = t.events.empty() ? t.target : t.events[0].id;
        loop_ = ecy_.states[size_t(state_)].type != 0;
        rate_ = t.rate > 0 ? t.rate : 30.f;  // the transition's playback rate (keys a second)
        // cycles hand over at the same phase (walk <-> run); a kind-1 transition from elsewhere starts the cycle
        // at its event's phase (stand -> walk: 0.3); one-shots start at the beginning
        if (loop_ && !fromCycle && t.kind == 1 && !t.events.empty()) phase = t.events[0].b;
        frame_ = loop_ ? phase * float(std::max(1, scene_->animCycle(object_, set_))) : 0.f;
        // FUN_10064d20: entering the jump states starts the thrust, 0.45 s x 0.5 for Jump (+0x1068 x
        // DAT_101433e8), the whole 0.45 s for Jump Jets
        if (state_ == sJumpAir_ && sJumpAir_ >= 0) thrust_ = 0.45f * 0.5f;
        else if (state_ == sJetAir_ && sJetAir_ >= 0) {
            thrust_ = 0.45f;
            if (particles) particles->spawn("JUMP_JET", pos_, 1.f);  // FUN_10065b00(0)
        }
        return true;
    }
    return false;
}

void Player::hitReaction(int level) {
    if (dead_ || object_ < 0 || size_t(state_) >= ecy_.states.size() || reactSet_ >= 0) return;
    int trig = level >= 4 ? 14 : level == 3 ? 13 : 12;
    for (auto& t : ecy_.states[size_t(state_)].transitions)
        if (t.trigger == trig && t.kind == 3 && !t.events.empty() && !t.tag.empty()) {
            reactSet_ = t.events[0].id, reactFrame_ = 0, reactRoot_ = t.tag;
            if (getenv("HG2_DEBUG")) SDL_Log("hit reaction: object %d level %d set %d on %s (%d keys)", object_, level, reactSet_, t.tag.c_str(), scene_->animLength(object_, reactSet_));
            return;
        }
}

void Player::knockDown(int trigger) {
    if (dead_ || object_ < 0 || ecy_.states.empty()) return;
    fire(trigger);
}

bool Player::knockedDown() const {
    if (size_t(state_) >= ecy_.states.size() || state_ <= sJetLand_) return false;
    // the fall / knock-down states are the ones left only by Stand Up (trigger 7) back to the stance
    for (auto& t : ecy_.states[size_t(state_)].transitions)
        if (t.trigger == kTrigUp && t.target == sIdle_) return true;
    return false;
}

void Player::revive() {
    crippled_ = crippleWant_ = false, impact_ = 0;
    dead_ = false, speed_ = 0, strafe_ = 0, vy_ = 0, yawRate_ = 0, thrust_ = 0, air_ = false, skateReq_ = false;
    reactSet_ = -1;
    if (scene_ && object_ >= 0) scene_->setObjectOverlay(object_, -1, 0, "");
    resolveStates();
    frame_ = 0;
}

void Player::die() {
    if (dead_ || object_ < 0) return;
    dead_ = true, speed_ = 0, strafe_ = 0, yawRate_ = 0, skateReq_ = false;
    if (reactSet_ >= 0) reactSet_ = -1, scene_->setObjectOverlay(object_, -1, 0, "");
    if (ecy_.states.empty()) return;
    int trig = 16 + rand() % 5;  // the five falls (forward, back, sideways, crumple)
    if (!fire(trig)) {
        state_ = sIdle_, loop_ = true;
        fire(trig);
    }
    loop_ = false, frame_ = 0;
}

int Player::postureCode() const {
    if (skating()) return 3;
    if (state_ >= 0 && (state_ == sProne_ || state_ == sCrawl_)) return 0;
    if (state_ >= 0 && (state_ == sKneel_ || state_ == sKneelWalk_)) return 2;
    return 1;
}

float Player::eyeHeight() const {
    int p = postureCode();
    return p == 0 ? 1.6f : p == 2 ? 3.4f : 5.0f;
}

float Player::vmax() const {
    return std::max(0.5f, (skating() ? vmaxSkate_ : vmaxWalk_) * std::max(0.05f, speedScale));
}

bool Player::movingState() const {
    // FUN_10064d20 sets +0x124a for the locomotion, posture-walk, jump and skate states and clears it for the
    // stances (stand, crouch, prone) and anything it doesn't list (falls, knock-downs)
    if (!canWalk()) return true;
    if (state_ == sIdle_ || state_ == sKneel_ || state_ == sProne_) return false;
    for (int s : {sWalk_, sRun_, sKneelWalk_, sCrawl_, sJump_, sJumpAir_, sJumpLand_, sJet_, sJetAir_, sJetLand_,
                  sSkateOn_, sSkate_, sSkateOff_})
        if (s >= 0 && state_ == s) return true;
    return false;
}

void Player::update(float dt, const PlayerInput& in) {
    if (object_ < 0) return;
    if (dead_) {  // play the fall once and stay down
        int len = std::max(1, scene_->animLength(object_, set_));
        frame_ = std::min(frame_ + rate_ * dt, float(len - 1));
        if (scene_->hasTerrain()) pos_.y = scene_->groundHeight(pos_.x, pos_.z);
        scene_->setObjectPose(object_, pos_, yaw_, set_, frame_, pitch_, roll_);  // a wreck keeps its tilt
        scene_->setObjectTwist(object_, 0);
        return;
    }
    const bool ecy = canWalk();
    float thr = lockForward_ ? 0.f : in.throttle, str = lockForward_ ? 0.f : in.strafe;
    // FUN_10063a10: steering with no forward input drives at throttle 0.05; reverse is held to -0.5
    if (std::fabs(in.turn) > 0.05f && std::fabs(thr) < 0.05f && !lockForward_) thr = 0.05f;
    thr = std::clamp(thr, -0.5f, 1.f);
    if (!pilot && !in.run && thr > 0) {
        // AI / remote bodies ask for walk or run: walking is the throttle that holds the walk speed on the flat
        // (drive = resistance: (v / vmax)^2), the AI's own pacing
        float w = walkLimit_ / vmax();
        thr = std::min(thr, w * w);
    }
    int len = std::max(1, scene_->animLength(object_, set_));
    bool done = !loop_ && frame_ >= float(len - 1);
    bool settled = loop_ || done;
    bool standing = state_ == sIdle_ || state_ == sWalk_ || state_ == sRun_;

    // trigger 15 (FUN_10062430 sends it with keep = 1 through the ECY controller, FUN_100164e0): every stance, posture
    // walk and fall of group 0 has a transition into the crippled group (0 -> 23, walk / run -> 24, crouch -> 25,
    // prone -> 26, falls 19 / 20 -> 33 / 34); skating 9 has 9 -> 8 (skates off, still group 0). The controller's
    // update (FUN_100340d0) drops a trigger that finds no transition in the current state (it pops the kept entry
    // too), so one sent mid-jump / jets / airborne (12 .. 18), while the skates go on (7) or during a knock-down
    // (21 / 22) is lost: the gear lands with Movement 1 but in group 0. A kept trigger whose transition is refused
    // is retried, which the port approximates by waiting for the current animation to settle.
    if (ecy && crippleWant_ && !crippled_ && stateOf(state_, kCrippleTrig) < 0) {
        crippleWant_ = false;
        if (getenv("HG2_DEBUG")) SDL_Log("object %d: trigger 15 dropped in state %d", object_, state_);
    }
    if (ecy && crippleWant_ && !crippled_ && settled) {
        int base = stateOf(0, kCrippleTrig);
        int target = stateOf(state_, kCrippleTrig);
        if (base < 0) {
            crippleWant_ = false;  // no crippled set in this .ECY
        } else if (fire(kCrippleTrig)) {
            crippleWant_ = false;
            // 9 -> 8 takes the trigger and stays in group 0 (the crippled group mirrors 0 .. 14 at base .. base + 13)
            if (target >= base && target <= base + 13) crippled_ = true, skateReq_ = false, wantCycle_ = -1;
            if (crippled_) resolveStates(base, false);
            if (getenv("HG2_DEBUG")) SDL_Log("object %d crippled: state %d (stand %d walk %d run %d kneel %d prone %d jets %d)", object_, state_, sIdle_, sWalk_, sRun_, sKneel_, sProne_, sJet_);
            len = std::max(1, scene_->animLength(object_, set_));
            done = !loop_ && frame_ >= float(len - 1);
            settled = loop_ || done;
            standing = state_ == sIdle_ || state_ == sWalk_ || state_ == sRun_;
        }
    }
    if (ecy && !air_) {
        // Wheels (FUN_10063a10 / FUN_10064700): the request (+0x1314) waits until the speed is under a quarter of
        // vmax, with the throttle cut meanwhile, then sends trigger 4; gears without a second Movement have no skates
        if (in.toggleSkates && def_.movement2 > 0) skateReq_ = true;
        if (skateReq_) {
            if (std::fabs(speed_) <= vmax() * 0.25f) {
                if ((standing || state_ == sSkate_) && settled) {
                    skateReq_ = false;
                    fire(kTrigWheels);
                }
            } else {
                thr = 0;
            }
        }
        bool down = in.down, up = in.up;
        if (wantPosture_ >= 0 && std::fabs(thr) < 0.05f && settled && !skating()) {  // scripted posture (SetStandingMode)
            int cur = postureCode(), rank[3] = {2, 0, 1};  // prone 2, stand 0, kneel 1 steps below standing
            if (rank[wantPosture_ % 3] > rank[cur % 3]) down = true;
            else if (rank[wantPosture_ % 3] < rank[cur % 3]) up = true;
        }
        if (!pilot && knockedDown() && done) up = true;  // AI bodies get up once the fall has played
        // Crouch / Stand Up go to the state machine as they are (it ignores them where the ECY has no transition);
        // Jump needs the ground and no thrust left, Jump Jets also the perk
        if (down && !skating() && settled) fire(kTrigDown);
        else if (up && settled) fire(kTrigUp);
        else if (in.jump && thrust_ <= 0 && (state_ == sWalk_ || state_ == sRun_)) fire(kTrigJump);
        else if (in.jumpJets && jumpJets > 0 && thrust_ <= 0 && standing) fire(kTrigJets);
        len = std::max(1, scene_->animLength(object_, set_));
        done = !loop_ && frame_ >= float(len - 1);
        settled = loop_ || done;
        // locomotion (FUN_10064700): trigger 1 (stop) once there is no throttle, turn or sidestep and |v| is under
        // 1 % of vmax, else 2 (walk) up to the walk limit (+0x10b4) and 3 (run) above it. Walk <-> run waits for one
        // of the transition's sync points (see advanceCycle). The posture walks take the same triggers.
        const float vabs = std::fabs(speed_);
        const bool rolling = std::fabs(thr) > 0.05f || std::fabs(str) > 0.05f || std::fabs(in.turn) > 1e-5f ||
                             vabs > 0.01f * vmax();
        wantCycle_ = -1;
        if (settled) {
            if (state_ == sIdle_ && rolling) fire(kTrigWalk);
            else if ((state_ == sWalk_ || state_ == sRun_) && !rolling) fire(kTrigStop);
            else if (state_ == sWalk_ && sRun_ >= 0 && vabs > walkLimit_) wantCycle_ = kTrigRun;
            else if (state_ == sRun_ && vabs <= walkLimit_) wantCycle_ = kTrigWalk;
            else if (state_ == sKneel_ && rolling && sKneelWalk_ >= 0) fire(kTrigWalk);
            else if (state_ == sKneelWalk_ && !rolling) fire(kTrigStop);
            else if (state_ == sProne_ && rolling && sCrawl_ >= 0) fire(kTrigWalk);
            else if (state_ == sCrawl_ && !rolling) fire(kTrigStop);
        }
        // one-shot chains: take-off -> in the air (held until the feet are down again) -> landing -> run; skates
        // going on -> skating, coming off -> walk
        if (done) {
            if (state_ == sJump_ || state_ == sJumpLand_) fire(kTrigJump);
            else if (state_ == sJet_ || state_ == sJetLand_) fire(kTrigJets);
            else if ((state_ == sJumpAir_ || state_ == sJetAir_) && thrust_ <= 0) fire(state_ == sJumpAir_ ? kTrigJump : kTrigJets);
            else if (state_ == sSkateOn_ || state_ == sSkateOff_) fire(kTrigWheels);
        }
    }

    const float m = float(def_.maneuverability);
    // FUN_1003aaf0 / FUN_1003c0b0: the turn command is the input x (0.6 + 0.05 Maneuverability), with the
    // Maneuverability as criticals have left it, held to +-1 (FUN_1005f1c0)
    const float mNow = maneuverNow == 0x7fff ? m : float(maneuverNow);
    float turnCmd = std::clamp(in.turn * (0.6f + 0.05f * mNow), -1.f, 1.f);
    // models face -Z; world = local * rotY(yaw). With the left-handed view, screen right is (-cos, 0, sin).
    Vec3 fwd(-std::sin(yaw_), 0, -std::cos(yaw_)), right(-std::cos(yaw_), 0, std::sin(yaw_));
    // sidestep (FUN_1003a9c0, run each physics step before the forces): Side Step Left / Right (FUN_10063a10, only in
    // the stand / walk / run states, on the ground) swing the leg angle at 300 deg/s (5.235987) to +-90 deg; left with
    // the throttle at or above 0 (right in reverse) turns the root left. Released, it comes back at the same rate and
    // snaps to 0 inside 1/128 rad. The angle is smoothed (+0x10c0: y = 0.8 y + 0.2 x a step; taken here at 30 steps a
    // second) and, with a torso joint (+0x11f0), FUN_1003c270 then sets the yaw rate to the smoothed swing over the
    // step, so the root (and the drive along it) turns with the legs while the torso joint takes the opposite angle
    // and keeps facing ahead. While a sidestep key is held the root cannot otherwise turn (yaw rate 0 once the swing
    // stops). There is no sideways force on the ground; the gear walks along its turned root with the throttle.
    const bool walkState = !ecy || state_ == sIdle_ || state_ == sWalk_ || state_ == sRun_;
    int legMode = 0;  // +0x13aa: 1 swing right, 2 swing left (port sense)
    if (!air_ && walkState && std::fabs(str) > 0.05f) legMode = (str > 0) == (thr >= 0) ? 1 : 2;
    {
        const float rate = 5.235987f, maxA = 1.5707963f;
        if (legMode == 1) legTarget_ = std::min(legTarget_ + rate * dt, maxA);
        else if (legMode == 2) legTarget_ = std::max(legTarget_ - rate * dt, -maxA);
        else if (legTarget_ > 1e-5f) { legTarget_ -= rate * dt; if (legTarget_ < 0.0078125f) legTarget_ = 0; }
        else if (legTarget_ < -1e-5f) { legTarget_ += rate * dt; if (legTarget_ > -0.0078125f) legTarget_ = 0; }
        legsPrev_ = legs_;
        legs_ += (legTarget_ - legs_) * (1.f - std::pow(0.8f, std::min(dt, 0.1f) * 30.f));
    }
    {
        // yaw: torque +0xf50 = 25 + 2 m (FUN_10035460 at 0x10035c65: 25 - m x -2, DAT_10140b68 / b60) x the turn
        // command (FUN_1003c070, x the inertia +0xda8 that the integrator divides out), damping 10 x rate (+0x1010,
        // FUN_1003d390) and, with no turn input on the ground, another 3 x rate (FUN_1003c750: 30 x -0.1)
        float sdt = std::min(dt, 0.1f);
        float a = (25.f + 2.f * m) * turnCmd - 10.f * yawRate_;
        if (std::fabs(turnCmd) < 0.05f && !air_) a -= 3.f * yawRate_;
        yawRate_ += a * sdt;
        if (dt > 0 && std::fabs(legs_ - legsPrev_) > 1e-5f) yawRate_ = (legs_ - legsPrev_) / dt;
        else if (legMode) yawRate_ = 0;
        yaw_ += yawRate_ * dt;
    }
    float targetStrafe = 0;  // slides only: the sidestep turns the root instead
    if (!air_) drive(dt, thr, targetStrafe, std::fabs(turnCmd) > 1e-5f, ecy, fwd, right);
    applyContacts(dt, fwd, right);
    if (space && air_ && pilot) {
        // FUN_1003c480 (space physics only, with no contact and off the ground): thrusters, (3 + 0.25 Maneuver)
        // (+0x1028) x the command (+0x1030 / +0x1040, FUN_10063a10 while airborne in space): Throttle Increase /
        // Decrease +-1 with the throttle setting added (forward mode) or taken off (reverse) along the gear's forward
        // axis, the sidestep keys +-2 sideways
        float k = 3.f + 0.25f * float(def_.maneuverability) , sdt = std::min(dt, 0.1f);
        float cz = in.thrust;
        float cx = in.strafe > 0.5f ? 2.f : in.strafe < -0.5f ? -2.f : 0.f;
        speed_ += k * cz * sdt, strafe_ += k * cx * sdt;
    }
    // jump thrust (FUN_1003af10 while +0x1070 > 0): 3.5 g up (+0x1060) and 0.2 of it forward, in the body's frame
    if (thrust_ > 0) {
        float t = std::min(dt, thrust_);
        thrust_ -= dt;
        vy_ += 3.5f * kGravity * t;
        speed_ += 0.7f * kGravity * t;
        air_ = true;
    }

    // animation: advance the set, and on the ground let a locomotion cycle's root motion carry the body
    if (!ecy) {  // no state machine: plain stand / walk / run sets
        int want = std::fabs(speed_) < 0.3f ? 0 : (speed_ > walkLimit_ ? 2 : 1);
        if (want != set_) set_ = want, frame_ = 0;
        loop_ = true;
    }
    len = std::max(1, loop_ ? scene_->animCycle(object_, set_) : scene_->animLength(object_, set_));
    bool cycle = !ecy ? set_ != 0
                      : loop_ && !skating() && !air_ && state_ != sIdle_ && state_ != sKneel_ && state_ != sProne_;

    int shown = set_;
    Vec3 travel;  // model-space root motion this step
    float travelShare = 0;  // the part of it that is the body's forward motion
    if (skating()) {
        frame_ = loop_ ? std::fmod(frame_ + rate_ * dt, float(len)) : std::min(frame_ + rate_ * dt, float(len - 1));
    } else if (cycle) {
        // FUN_10064d20: the cycle plays at speed / stride cycles a second (FUN_10055290 with FUN_100344e0's inverse
        // stride), and the body moves by the root's travel over the step, not by velocity x dt
        float v = std::max(std::fabs(speed_), std::fabs(strafe_));
        travelShare = v > 1e-3f ? std::fabs(speed_) / v : 0.f;
        travel = advanceCycle(dt, std::copysign(v, speed_ < -0.1f ? -1.f : 1.f));
        shown = set_, len = std::max(1, scene_->animCycle(object_, set_));
    } else {
        if (idleSet_ != set_ && state_ == sIdle_) frame_ = 0, idleSet_ = set_;
        if (loop_) frame_ = std::fmod(frame_ + rate_ * dt + float(len), float(len));
        else frame_ = std::min(frame_ + rate_ * dt, float(len - 1));
    }

    // horizontal motion: root motion for a walking cycle (scaled to the forward share when sidestepping), velocity
    // otherwise; sideways velocity (sidestep, slides) always
    if (cycle && !air_ && travelShare > 0 && shown == set_) {
        Vec3 w = Mat4::rotY(yaw_).transformDir(travel);
        pos_ += Vec3(w.x, 0, w.z) * travelShare + right * (strafe_ * dt);
    } else {
        pos_ += fwd * (speed_ * dt) + right * (strafe_ * dt);
    }
    pos_.x = std::max(1.f, std::min(4095.f, pos_.x));
    pos_.z = std::max(1.f, std::min(4095.f, pos_.z));
    // vertical (FUN_1003cc30): the ground is the support the downward probes find (terrain or an object's top);
    // gravity acts all the time and the contact stops the fall. The body counts as grounded until it is more than
    // 2 m above the ground (the player's gear: 0.5 m).
    const bool biped = chassis_ == kBiped;
    float ground = biped ? support(supportN_) : 0.f;
    const float radius = this->radius();
    // gravity (FUN_1003d1f0, 20 m/s^2, always); in space (FUN_1003b850) there is none, only a 1 g pull along the
    // normal of a surface the body is in contact with (+0xfe0, contacts within the bounding volume). The vehicle
    // chassis apply it in chassisStep.
    if (biped && (!space || pos_.y - ground <= radius)) vy_ -= kGravity * dt;
    // FUN_10057e90 (vtable +0x278): drag on the whole velocity, 0.5 v^2 x 20 (+0xff8), x 400 d / r while the body's
    // origin is d under the water (d at most r, the bounding radius +0x98); never more than stops the body this step.
    // Forces are divided by the mass 100 r^3 (+0xd98, FUN_10035460), so out of the water it is negligible and
    // wading / swimming is slowed in proportion to the depth.
    {
        float v2 = speed_ * speed_ + strafe_ * strafe_ + vy_ * vy_, v = std::sqrt(v2), sdt = std::min(dt, 0.1f);
        if (v > 1e-5f && sdt > 0) {
            float k = 0.5f * v2 * 20.f;
            if (scene_->mission().hasWater) {
                float d = std::min(scene_->waterLevel() - pos_.y, radius);
                if (d > 0) k *= 400.f * d / radius;
            }
            float a = std::min(k / (massK_ * radius * radius * radius), v / sdt);
            float f = 1.f - a * sdt / v;
            speed_ *= f, strafe_ *= f, vy_ *= f;
        }
    }
    if (!biped) {
        chassisStep(dt);
    } else {
        pos_.y += vy_ * dt;
        if (pos_.y <= ground) {
            bool landed = air_;
            if (vy_ < 0) impact_ = std::max(impact_, -vy_);  // FUN_1003cc30: the hard-landing test (Combat::hardLanding)
            pos_.y = ground, vy_ = std::max(0.f, vy_), air_ = false;
            if (landed) {
                footfall();  // landing: the surface's footstep sound and dust (FUN_10067570)
            }
        } else if (pos_.y - ground > (pilot ? 0.5f : 2.f)) {
            air_ = true;  // walked off an edge
        }
    }

    scene_->setObjectPose(object_, pos_, yaw_, shown, frame_, pitch_, roll_);
    gatherContacts();
    if (getenv("HG2_FOOTDBG") && object_ == atoi(getenv("HG2_FOOTDBG"))) {
        Vec3 l, r;
        const char* fl = getenv("HG2_FOOTL") ? getenv("HG2_FOOTL") : "frm-lld"; const char* frr = getenv("HG2_FOOTR") ? getenv("HG2_FOOTR") : "frm-rld";
        if (scene_->frameBottom(object_, fl, l) && scene_->frameBottom(object_, frr, r))
            SDL_Log("foot %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f speed %.2f set %d frame %.1f side %.2f slope %.1f yaw %.4f state %d air %d", pos_.x, pos_.y, pos_.z, l.x, l.y, l.z,
                    r.x, r.y, r.z, speed_, shown, frame_, strafe_, std::acos(std::min(1.f, scene_->groundNormal(pos_.x, pos_.z).y)) / kDeg, yaw_, state_, int(air_));
    }
    if (reactSet_ >= 0) {
        reactFrame_ += 30.f * dt;
        if (reactFrame_ >= float(scene_->animLength(object_, reactSet_))) {
            reactSet_ = -1;
            scene_->setObjectOverlay(object_, -1, 0, "");
        } else {
            scene_->setObjectOverlay(object_, reactSet_, reactFrame_, reactRoot_);
        }
    }
    scene_->setObjectTwist(object_, twist - legs_);  // the torso joint holds the sidestep's opposite angle
    if (boundR_ <= 0) {  // the body's bounding sphere, once posed (unit +0x80 / +0x98)
        float r = scene_->objectRadius(object_);
        Vec3 c;
        if (r > 0 && scene_->objectCenter(object_, c)) {
            boundC_ = Mat4::rotY(-yaw_).transformDir(c - pos_);
            boundR_ = r;
        }
    }
    if (cycle && !air_) sounds();
    else stepPrev_ = false;
    if (audio && engineLoop) {
        if (motor_ < 0 && !def_.soundMotor.empty()) motor_ = audio->startLoop(def_.soundMotor, 0.f);
        float v = std::min(1.f, std::fabs(speed_) / vmax());
        audio->setLoop(motor_, soundVolume * (0.12f + 0.3f * v), &pos_, 400.f);
    }
}

void Player::drive(float dt, float throttle, float targetStrafe, bool turning, bool ecy, const Vec3& fwd,
                   const Vec3& right) {
    // The ground part of the physics step FUN_1003af10 (gear class vtable 0x101434a0, constants set in FUN_10060270
    // from the gear's Maneuverability m), as accelerations on the local velocity (forward speed_, sideways strafe_).
    // The step is at most 1/10 s.
    dt = std::min(dt, 0.1f);
    if (dt <= 0) return;
    const float m = float(def_.maneuverability);
    // FUN_10035460 at 0x10035c38: +0xf58 = m - (-20) (DAT_10140b58), copied to +0xf60
    const float drivek = 20.f + m;          // +0xf58: drive per unit throttle
    const float dragk = 20.f + m;           // +0xf60: forward resistance
    const float sidek = 400.f + 30.f * m;   // +0xf68: sideways resistance
    const float g = kGravity, mu = 2.f;     // DAT_10163ae8, +0xff0
    const float vm = vmax();                // +0x270
    const bool hasThrottle = std::fabs(throttle) >= 0.05f;
    // FUN_1003cc30 (vtable +0x268), the player's gear only (+0x1f0; AI bodies get none): with the ground normal n
    // (+0xfc0: the terrain triangle under the body's centre, or the polygon of an object stood on) in the gear's frame, n_h |n_h| x k x g on each horizontal axis,
    // k = min(3 / n_y^2, 6) (skates min(5 / n_y^2, 9)). On ground flatter than n_y 0.8 (36.9 deg) the sideways part is
    // dropped, and the forward part too with no throttle. It is added after the traction cap below, so with the drive
    // at most 20 a gear can't climb past ~30 deg (60 tan^2 = 20), and on faces steeper than 36.9 deg the slope beats
    // the 40 of traction and the gear slides until the ground levels out.
    float sF = 0, sR = 0;
    if (ecy && pilot) {
        Vec3 n = supportN_;
        float ny = n.y < 1e-5f ? 1.f : n.y;
        float k = skating() ? std::min(5.f / (ny * ny), 9.f) : std::min(3.f / (ny * ny), 6.f);
        float nf = dot(n, fwd), nr = dot(n, right);
        sF = nf * std::fabs(nf) * k * g, sR = nr * std::fabs(nr) * k * g;
        if (ny > 0.8f) {
            sR = 0;
            if (!hasThrottle) sF = 0;
        }
    }
    // FUN_1003c0b0: forward drive = drivek x throttle (the command is already held to -0.5 .. 1)
    float aF = drivek * throttle;
    // FUN_1003c5f0: forward resistance on r = v / vmax, r |r| plus 2 r with no throttle; r x 4 in the posture walks
    // (+0x1249), r x 10 while the body isn't in a moving state (+0x124a clear: stand, crouching), so a standing gear
    // stops dead. Past r = 2 the velocity is scaled back by 1 / r. It never reverses the speed within a step.
    float r = speed_ / vm;
    if (ecy && (state_ == sKneelWalk_ || state_ == sCrawl_)) r *= 4.f;
    if (ecy && !movingState()) r *= 10.f;
    if (r > 2.f) speed_ /= r, strafe_ /= r;
    float drag = dragk * (r * std::fabs(r) + (hasThrottle ? 0.f : 2.f * r));
    drag = std::copysign(std::min(std::fabs(drag), std::fabs(speed_) / dt), drag);
    aF -= drag;
    // FUN_1003c750: sideways resistance sidek x v_side / vmax (v_side clamped to +-20), never reversing within a step;
    // the port's sidestep is a commanded sideways speed, so it resists the difference
    float vs = strafe_ - targetStrafe;
    float aR = -sidek * std::clamp(vs, -20.f, 20.f) / vm;
    aR = std::copysign(std::min(std::fabs(aR), std::fabs(vs) / dt), aR);
    // FUN_1003c8f0: drive and resistance together are capped at the traction mu x g; the slope force comes after
    float mag = std::hypot(aF, aR);
    if (mag > mu * g) aF *= mu * g / mag, aR *= mu * g / mag;
    aF += sF, aR += sR;
    speed_ += aF * dt;
    strafe_ += aR * dt;
    // FUN_1003af10: with no throttle or turn and under 0.05 m/s the body is put at rest
    if (!hasThrottle && !turning && std::fabs(targetStrafe) < 1e-3f && std::hypot(speed_, strafe_) < 0.05f) speed_ = 0, strafe_ = 0;
}

float Player::radius() {
    if (radius_ < 0) radius_ = scene_->boundRadius(object_);
    return radius_;
}

// The pass tests a unit against other units only in pairs that hold the player (FUN_100a0b00, DAT_101fcf48): the
// player's gear against every unit, the others against the player's gear alone.
int Player::unitFilter() const {
    int pl = scene_->playerObject();
    return pl < 0 ? -1 : pl == object_ ? -2 : pl;
}

// FUN_1003db20 on the probes of FUN_1003d640 (biped gear vtable +0x28c; X, Y, Z, T the rows of the gear's matrix
// +0xdb8, r its bounding radius): segment 0 runs from T + 2Y down to T - 15Y, segments 1 / 2 from T +- 1.5X + 2Y down
// to T +- 1.5X - rY. The support is the highest crossing above the terrain under the body's centre whose polygon
// faces up (n . Y > 0.7, DAT_10140cf0); with none it is the terrain (FUN_10095940). Only polygons crossed from their
// front count, so a probe starting inside or under a model's floor passes up through it.
float Player::support(Vec3& normal) {
    float best = scene_->groundHeight(pos_.x, pos_.z);
    normal = scene_->groundNormal(pos_.x, pos_.z);
    const float r = radius();
    Vec3 X = Mat4::rotY(yaw_).transformDir(Vec3(1, 0, 0)), Y(0, 1, 0);
    const Vec3 from[3] = {pos_ + Y * 2.f, pos_ + X * 1.5f + Y * 2.f, pos_ - X * 1.5f + Y * 2.f};
    const Vec3 to[3] = {pos_ - Y * 15.f, pos_ + X * 1.5f - Y * r, pos_ - X * 1.5f - Y * r};
    const int units = unitFilter();
    for (int k = 0; k < 3; k++) {
        Scene::SegmentHit h;
        if (!scene_->segment(from[k], to[k], object_, units, h)) continue;
        float y = from[k].y + (to[k].y - from[k].y) * h.t;
        if (y > best && h.n.y > 0.7f) best = y, normal = h.n;
    }
    return best;
}

void Player::gatherContacts() {
    contacts_.clear();
    const float r = radius();
    if (r <= 0) return;
    const int units = unitFilter();
    const Mat4 R = bodyRotation();
    Vec3 Y = chassis_ == kBiped ? Vec3(0, 1, 0) : R.transformDir(Vec3(0, 1, 0));
    Vec3 Z = R.transformDir(Vec3(0, 0, 1));  // models face -Z
    // The forward probe. Bipeds (FUN_1003d640 segment 3): from T + Y forward and up to T + (1.25r + 1)Y - 1.25r Z
    // (DAT_10140cd0 = 1.25). Hovertanks (FUN_1006a140 segment 1): to T + (1.25r + 1)Y - 2r Z. Jeeps, Mounts and
    // planes have none. A crossing makes a contact pushing back along the body's +Z, depth 1, spring 40, damping 30
    // (FUN_1003db20, FUN_1006a2c0).
    if (chassis_ == kBiped || chassis_ == kHover) {
        float reach = chassis_ == kHover ? 2.f * r : 1.25f * r;
        Vec3 a = pos_ + Y, b = pos_ + Y * (1.25f * r + 1.f) - Z * reach;
        Scene::SegmentHit h;
        if (scene_->segment(a, b, object_, units, h)) {
            contacts_.push_back({a + (b - a) * h.t, Z, 1.f, 40.f, 30.f});
            if (getenv("HG2_CONTACTDBG") && atoi(getenv("HG2_CONTACTDBG")) == object_)
                SDL_Log("contact %d front probe: object %d (%s) at t %.2f, pos %.0f,%.0f", object_, h.object,
                        h.object >= 0 ? scene_->mission().objects[size_t(h.object)].name.c_str() : "", h.t, pos_.x, pos_.z);
        }
    }
    // FUN_100a63d0's second loop, for units within 50 m of the player (2500 = DAT_10147080) and the player itself:
    // a segment from the body's centre to each mesh part's origin, tested against objects whose box holds one of its
    // ends; a crossing at t gives a contact along the polygon's normal with depth 1 - t; spring 40 / damping 10 for
    // the biped classes (FUN_1003db20), 30 / 30 for Jeeps, hovertanks, Mounts and planes (FUN_10081710, FUN_1006a2c0,
    // FUN_10071df0, FUN_10058320). A crossing into another unit is a part hit between two dynamic proxies: the
    // pair's ramming damage (Combat::ram).
    Vec3 pp;
    int pl = scene_->playerObject();
    bool near = pl == object_ || (pl >= 0 && scene_->objectPosition(pl, pp) && dot(pp - pos_, pp - pos_) < 2500.f);
    if (!near) return;
    Vec3 centre;
    std::vector<Vec3> ends;
    if (!scene_->bodySegments(object_, centre, ends)) return;
    const float k = chassis_ == kBiped ? 40.f : 30.f, c = chassis_ == kBiped ? 10.f : 30.f;
    for (const Vec3& e : ends) {
        if (contacts_.size() >= 19) break;
        Scene::SegmentHit h;
        if (scene_->segment(centre, e, object_, units, h, true) && h.t > 0.f && h.t < 1.f) {
            contacts_.push_back({centre + (e - centre) * h.t, h.n, 1.f - h.t, k, c});
            if (h.object >= 0 && scene_->isUnit(h.object) &&
                std::find(rams_.begin(), rams_.end(), h.object) == rams_.end())
                rams_.push_back(h.object);
            if (getenv("HG2_CONTACTDBG") && atoi(getenv("HG2_CONTACTDBG")) == object_)
                SDL_Log("contact %d part: object %d (%s) depth %.2f n %.2f %.2f %.2f, pos %.0f,%.0f", object_, h.object,
                        h.object >= 0 ? scene_->mission().objects[size_t(h.object)].name.c_str() : "", 1.f - h.t, h.n.x, h.n.y,
                        h.n.z, pos_.x, pos_.z);
        }
    }
}

Mat4 Player::bodyRotation() const {
    return Mat4::rotZ(roll_) * Mat4::rotX(pitch_) * Mat4::rotY(yaw_);
}

// Per-class setup (the class constructors / init functions):
// - Jeep (0x4e; FUN_1007e040, the wheeled / tracked vehicle base, vtable 0x10146188 -> 0x101449b8): mass 10 r^3
//   (+0xd98), up to six wheel records 0x180 bytes apart from +0x10b0, placed at the model frames fr / fl / mr / ml /
//   br / bl (else frt / flt / mrt / mlt / brt / blt; else the defaults (+-r/2, 0, +-r/2)). A wheel's radius is half
//   its frame mesh's height, at least 0.75 (DAT_101460d8; 0.2 r for a default wheel); its suspension length L =
//   0.2 x (Maneuverability + 7) x radius (DAT_10146108, +0xbf4 + 7) and its point sits at y = L / 2 (the frame's own
//   height is replaced), so at the rest compression 1/2 the body's origin is on the ground. Spring 10 m and damping
//   10 m per wheel (20 x 0.5 x mass, DAT_101460f0); with a middle wheel on a side the side's springs are x 2/3
//   (DAT_10146110). Torque gain +0x1000 = 30, angular damping +0x1008 / +0x1018 = 8. The probes (FUN_10081070, four,
//   six with both middle wheels): vertical from each wheel point 0.5 r above (DAT_101460f0) to 2.5 r below
//   (x DAT_10146158 = 5).
// - Mount (0x51; init FUN_1006f620): four leg records from +0x13d8 at (+-r/2, L/2, +-r/2), L = 0.2 r (at least 1),
//   spring 20 m, damping 4 m, torque gain 10, angular damping 5 (x / z). Probes (FUN_10071a10): 5 m above each leg
//   point to 5 m below.
// - Hovertank (0x4f; FUN_10067890's init): mass 100 r^3, align gain +0x1000 = 5, angular damping 2 (x / z).
void Player::setupChassis(int type) {
    wheels_.clear();
    chassis_ = type == 0x4e ? kWheels : type == 0x4f ? kHover : type == 0x51 ? kLegs : kBiped;
    pitch_ = roll_ = pitchRate_ = rollRate_ = 0;
    if (chassis_ == kBiped) return;
    const float r = std::max(0.1f, radius());
    if (chassis_ == kHover) {
        massK_ = 100.f, tiltK_ = 5.f, tiltD_ = 2.f, tiltDz_ = 2.f;
        return;
    }
    if (chassis_ == kLegs) {
        massK_ = 100.f, tiltK_ = 10.f, tiltD_ = 5.f, tiltDz_ = 5.f, probeUp_ = 5.f, probeDown_ = 5.f;
        float L = std::max(0.2f * r, 1.f);
        const float sx[4] = {1, -1, 1, -1}, sz[4] = {-1, -1, 1, 1};
        for (int i = 0; i < 4; i++) {
            Wheel w;
            w.p = Vec3(sx[i] * 0.5f * r, 0.5f * L, sz[i] * 0.5f * r);
            w.len = L, w.k = 20.f, w.c = 4.f, w.fb = i < 2 ? 1 : -1;
            wheels_.push_back(w);
        }
        return;
    }
    massK_ = 10.f, tiltK_ = 30.f, tiltD_ = 8.f, tiltDz_ = 8.f, probeUp_ = 0.5f * r, probeDown_ = 2.5f * r;
    const float m = float(maneuverNow == 0x7fff ? def_.maneuverability : maneuverNow);
    const char* names[6][2] = {{"fr", "frt"}, {"fl", "flt"}, {"mr", "mrt"}, {"ml", "mlt"}, {"br", "brt"}, {"bl", "blt"}};
    const float dx[6] = {0.5f, -0.5f, 0.5f, -0.5f, 0.5f, -0.5f}, dz[6] = {-0.5f, -0.5f, 0, 0, 0.5f, 0.5f};
    bool middle[2] = {false, false};
    for (int i = 0; i < 6; i++) {
        Wheel w;
        Vec3 o, lo, hi;
        bool found = scene_->frameRest(object_, names[i][0], o, lo, hi) || scene_->frameRest(object_, names[i][1], o, lo, hi);
        float rw;
        if (found) {
            rw = std::max(0.5f * (hi.y - lo.y), 0.75f);
        } else {
            if (i == 2 || i == 3) continue;  // no middle wheel: the record stays unused
            o = Vec3(dx[i] * r, 0, dz[i] * r), rw = 0.2f * r;
        }
        if (i == 2 || i == 3) middle[i - 2] = true;
        w.len = 0.2f * (m + 7.f) * rw;
        if (w.len < 0.05f) w.len = 0.05f;
        w.p = Vec3(o.x, 0.5f * w.len, o.z);
        w.k = 10.f, w.c = 10.f;
        w.fb = i < 2 ? 1 : i < 4 ? 0 : -1;
        wheels_.push_back(w);
    }
    for (auto& w : wheels_)  // the side with a middle wheel: springs x 2/3 (right fr / mr / br, left fl / ml / bl)
        if (w.p.x != 0 && middle[w.p.x < 0 ? 0 : 1]) w.k *= 2.f / 3.f;  // models face -Z: right is -X
    if (getenv("HG2_CHASSISDBG"))
        for (auto& w : wheels_)
            SDL_Log("chassis %d: wheel %.2f %.2f %.2f L %.2f k %.1f", object_, w.p.x, w.p.y, w.p.z, w.len, w.k);
}

// The vertical motion and tilt of the vehicle classes, at most 1/30 s a step.
// - Jeep / Mount (FUN_10081710 / FUN_10071df0 gather each wheel's ground, FUN_10080b00 / FUN_100714e0 sum the
//   springs, FUN_10080990 / FUN_10071370 lift the body out of the ground): the ground under a wheel is the terrain at
//   its world point (FUN_10095940, with its normal) unless the wheel's probe crossed an object higher up (a type 2
//   contact: that point and plane). d = the point's height above it; the compression c = 1 - min(max(d, 0), L) / L,
//   touching while d <= L. Each wheel pushes along the ground's normal with k c + the damping L c_d (c - c_prev) / dt,
//   the damping held to 0.05 |c - c_prev| / dt^2 (DAT_10146148, per unit mass). The deepest penetration (-d) lifts the
//   body at once and stops its fall. Torques (+0x1000 x inertia): about X the back corners' compression minus the
//   front's, about Z one side's minus the other's - the body turns until the corners share the load - with the
//   angular damping -w x +0x1008 / +0x1018. Only the force's vertical part is taken here (the port leaves the
//   springs' sideways push and the wheel friction out). Airborne while no wheel touches.
// - Hovertank (FUN_1006a2c0, FUN_1006a020, FUN_100698c0): the ground is the terrain under the body's origin, or the
//   water surface when that is higher (normal straight up, +0x1190), raised by a crossing of the downward probe
//   (FUN_1006a140 segment 0: T + 3 up to T - 20, world vertical). With d the origin's height above it: at d <= 0 the
//   body is put on it and stops falling; up to d = 3 (DAT_10144340) a lift of (1 - d / 3) x 3 g (DAT_10144348), so it
//   floats at d = 2; above d = 6 (DAT_10144338) any upward speed is cut; a vertical damping of -v_y (FUN_100698c0:
//   -(+0xee0) x mass). Its up axis is pulled toward the ground's normal plus 0.2 up (DAT_10144320, renormalised,
//   +0x27c = FUN_1002d560) with gain 5 and damping 2.
void Player::chassisStep(float dt) {
    const int units = unitFilter();
    float left = std::max(0.f, dt);
    while (left > 1e-6f) {
        const float h = std::min(left, 1.f / 30.f);
        left -= h;
        if (chassis_ == kHover) {
            float ground = scene_->groundHeight(pos_.x, pos_.z);
            Vec3 n = scene_->groundNormal(pos_.x, pos_.z);
            if (scene_->mission().hasWater && scene_->waterLevel() > ground) ground = scene_->waterLevel(), n = Vec3(0, 1, 0);
            Scene::SegmentHit sh;
            Vec3 a = pos_ + Vec3(0, 3.f, 0), b = pos_ - Vec3(0, 20.f, 0);
            if (scene_->segment(a, b, object_, units, sh)) {
                float y = a.y + (b.y - a.y) * sh.t;
                if (y > ground) ground = y;
            }
            supportN_ = n;
            float d = pos_.y - ground;
            if (d <= 0) {
                if (air_ && vy_ < 0) impact_ = std::max(impact_, -vy_);
                pos_.y = ground, d = 0;
                if (vy_ < 0) vy_ = 0;
            }
            if (d > 6.f && vy_ > 0) vy_ = 0;
            float ay = -kGravity - vy_;
            if (d <= 3.f) ay += (1.f - d / 3.f) * kGravity * 3.f;
            air_ = d > 3.f;
            vy_ += ay * h;
            pos_.y += vy_ * h;
            // alignment: rotate the body's up axis toward v (small-angle torque about the body's X and Z)
            Vec3 v = normalize(n + Vec3(0, 0.2f, 0));
            const float e = 1e-3f;
            auto upOf = [&](float p, float q) {
                return (Mat4::rotZ(q) * Mat4::rotX(p) * Mat4::rotY(yaw_)).transformDir(Vec3(0, 1, 0));
            };
            Vec3 up = upOf(pitch_, roll_);
            Vec3 dp = (upOf(pitch_ + e, roll_) - up) * (1.f / e), dr = (upOf(pitch_, roll_ + e) - up) * (1.f / e);
            pitchRate_ += (tiltK_ * dot(v - up, dp) - tiltD_ * pitchRate_) * h;
            rollRate_ += (tiltK_ * dot(v - up, dr) - tiltDz_ * rollRate_) * h;
        } else {
            const Mat4 R = bodyRotation();
            float pen = 0, fy = 0;
            bool any = false;
            Vec3 nsum;
            for (auto& w : wheels_) {
                Vec3 p = pos_ + R.transformDir(w.p);
                w.ground = scene_->groundHeight(p.x, p.z);
                w.n = scene_->groundNormal(p.x, p.z);
                Scene::SegmentHit sh;
                Vec3 a = p + Vec3(0, probeUp_, 0), b = p - Vec3(0, probeDown_, 0);
                if (scene_->segment(a, b, object_, units, sh)) {
                    float y = a.y + (b.y - a.y) * sh.t;
                    if (y > w.ground) w.ground = y, w.n = sh.n;
                }
                float d = p.y - w.ground;
                if (d < 0) pen = std::max(pen, -d);
                w.touch = d <= w.len;
                w.prev = w.comp;
                w.comp = 1.f - std::clamp(d, 0.f, w.len) / w.len;
                float dc = w.comp - w.prev;
                float damp = w.len * w.c * dc / h, cap = 0.05f * std::fabs(dc) / (h * h);
                damp = std::clamp(damp, -cap, cap);
                float f = w.k * w.comp + damp;
                fy += f * w.n.y;
                nsum += w.n;
                any = any || w.touch;
            }
            if (pen > 0) {  // FUN_10080990: the body is lifted out of the ground and stops falling
                pos_.y += pen;
                if (vy_ < 0) impact_ = std::max(impact_, -vy_), vy_ = 0;
            }
            air_ = !any;
            vy_ += (fy - kGravity) * h;
            pos_.y += vy_ * h;
            if (length(nsum) > 0) supportN_ = normalize(nsum);
            // front records minus back ones (whatever their z: a Jeep's origin can sit behind both axles), and one
            // side minus the other, signed by the side the point lies on so the turn always lifts the loaded side
            float tp = 0, tr = 0;
            for (auto& w : wheels_) {
                if (!w.fb) continue;
                tp += float(w.fb) * w.comp;
                tr += w.p.x > 0 ? w.comp : w.p.x < 0 ? -w.comp : 0.f;
            }
            pitchRate_ += (tiltK_ * tp - tiltD_ * pitchRate_) * h;
            rollRate_ += (tiltK_ * tr - tiltDz_ * rollRate_) * h;
        }
        pitch_ += pitchRate_ * h, roll_ += rollRate_ * h;
        pitch_ = std::clamp(pitch_, -1.2f, 1.2f), roll_ = std::clamp(roll_, -1.2f, 1.2f);
    }
    if (const char* dbg = getenv("HG2_CHASSISDBG")) {
        static float clock = 0;
        if ((object_ == atoi(dbg) || atoi(dbg) < 0) && int((clock + dt) * 2) != int(clock * 2)) {
            Vec3 n = scene_->groundNormal(pos_.x, pos_.z);
            SDL_Log("chassis %d: pos %.1f,%.2f,%.1f terrain %.2f slope %.1f deg pitch %.1f roll %.1f air %d vy %.2f speed %.1f",
                    object_, pos_.x, pos_.y, pos_.z, scene_->groundHeight(pos_.x, pos_.z), std::acos(std::min(1.f, n.y)) / kDeg,
                    pitch_ / kDeg, roll_ / kDeg, int(air_), vy_, speed_);
        }
        if (object_ == atoi(dbg) || atoi(dbg) < 0) clock += dt;
    }
}

// FUN_10058710: each contact pushes along its normal's horizontal part with spring x depth (the depth held to
// 0.2 .. 0.5), damps the velocity with c (on the ground; at most 100 and at most what stops it within 0.75 of the
// step), and takes away the part of the velocity that runs into the contact (when moving toward its point). The
// pushes together are held to 200.
void Player::applyContacts(float dt, const Vec3& fwd, const Vec3& right) {
    if (contacts_.empty() || dt <= 0) return;
    dt = std::min(dt, 0.1f);
    Vec3 v = fwd * speed_ + right * strafe_ + Vec3(0, vy_, 0);
    Vec3 f;
    for (const Contact& c : contacts_) {
        float depth = std::clamp(c.depth, 0.2f, 0.5f);
        f.x += c.k * depth * c.n.x, f.z += c.k * depth * c.n.z;
        if (!air_) {
            Vec3 d = v * -c.c;
            float cap = std::min(100.f, length(v) * 0.75f / dt), dl = length(d);
            if (dl > cap && dl > 0) d = d * (cap / dl);
            f.x += d.x, f.z += d.z;
        }
        float vn = dot(v, c.n);
        if (dot(c.p - pos_, v) > 0 && vn < 0) v = v - c.n * vn;
    }
    float fl = length(f);
    if (fl > 200.f) f = f * (200.f / fl);
    v = v + f * dt;
    speed_ = dot(v, fwd), strafe_ = dot(v, right), vy_ = v.y;
    contacts_.clear();
}

float Player::cycleStride(int set) {
    for (auto& [k, v] : strides_)
        if (k == set) return v;
    int len = std::max(1, scene_->animCycle(object_, set));
    Vec3 t = scene_->animTravel(object_, set, float(len));
    float s = std::hypot(t.x, t.z);
    strides_.push_back({set, s});
    return s;
}

Vec3 Player::advanceCycle(float dt, float v) {
    auto travel = [&](int set, int len, float from, float to) {  // root travel from key `from` to `to` (unwrapped)
        auto T = [&](float f) { return scene_->animTravel(object_, set, f); };
        if (to >= float(len)) return (T(float(len)) - T(from)) + T(to - float(len));
        if (to < 0) return (Vec3() - T(from)) + (T(to + float(len)) - T(float(len)));
        return T(to) - T(from);
    };
    auto rateFor = [&](int set, int len) {
        // FUN_10064d20 sets the cycle's rate to speed x 1 / stride (FUN_100344e0): one loop per stride travelled;
        // a cycle whose root travels 0.125 m or less counts as 1 / 8 (DAT_101408cc)
        float stride = cycleStride(set);
        if (stride <= 0.125f) stride = 0.125f;
        return v * float(len) / stride;
    };
    int len = std::max(1, scene_->animCycle(object_, set_));
    frame_ = std::fmod(std::max(0.f, frame_), float(len));
    float rate = rateFor(set_, len);
    float f0 = frame_, f1 = f0 + rate * dt;
    if (wantCycle_ >= 0 && size_t(state_) < ecy_.states.size()) {
        for (auto& t : ecy_.states[size_t(state_)].transitions) {
            if (t.trigger != wantCycle_ || t.target < 0 || size_t(t.target) >= ecy_.states.size()) continue;
            if (t.kind != 1 || t.events.empty()) {  // no sync points: switch now, at the same phase
                fire(wantCycle_);
                wantCycle_ = -1;
                return advanceCycle(dt, v);
            }
            // a kind-1 transition (walk <-> run) waits for the cycle to reach one of its events' phases a, and starts
            // the target at phase b (a = b = 0.05 / 0.3 / 0.55 / 0.8 on the gears: the foot plants)
            for (auto& e : t.events) {
                float p = e.a * float(len), hit = 0;
                bool found = false;
                for (float q : {p - float(len), p, p + float(len)})
                    if ((rate > 0 && q > f0 && q <= f1) || (rate < 0 && q < f0 && q >= f1)) hit = q, found = true;
                if (!found) continue;
                Vec3 out = travel(set_, len, f0, hit);
                float rest = rate != 0 ? (f1 - hit) / rate : 0.f;  // time left after the switch
                state_ = t.target, set_ = e.id, loop_ = ecy_.states[size_t(state_)].type != 0, wantCycle_ = -1;
                int nlen = std::max(1, scene_->animCycle(object_, set_));
                float g0 = std::fmod(e.b, 1.f) * float(nlen), g1 = g0 + rateFor(set_, nlen) * rest;
                out += travel(set_, nlen, g0, g1);
                frame_ = std::fmod(g1 + float(nlen), float(nlen));
                return out;
            }
            break;
        }
    }
    Vec3 out = travel(set_, len, f0, f1);
    frame_ = std::fmod(f1 + float(len), float(len));
    return out;
}

void Player::sounds() {
    // FUN_10067570, every step of a walking state on the ground: the cycle position p = key time / (keys - 10) (the
    // set's length without its padding) against the gear's LeftFootSound / RightFootSound points lo <= hi (+0xc08 /
    // +0xc0c, the model's +0x160 / +0x164): entering (lo, hi] is one foot fall (the sound 2 m to the body's -x), passing
    // hi the other (+x); each arms the other. A fall in two steps running only sounds once (+0x16a).
    int len = std::max(1, scene_->animCycle(object_, set_));
    float p = frame_ / float(len);
    float lo = std::min(def_.leftFoot, def_.rightFoot), hi = std::max(def_.leftFoot, def_.rightFoot);
    float side = 0;
    bool step = false;
    if (p <= hi) {
        if (lo < p && !stepIn_) step = true, side = -1, stepIn_ = true, stepPast_ = false;
    } else if (!stepPast_) {
        step = true, side = 1, stepPast_ = true, stepIn_ = false;
    }
    if (step && !stepPrev_) {
        if (getenv("HG2_FOOTDBG") && object_ == atoi(getenv("HG2_FOOTDBG"))) SDL_Log("footfall side %+.0f at p %.3f (lo %.2f hi %.2f)", side, p, lo, hi);
        footfall(side);
    }
    stepPrev_ = step;
}

// FUN_10067570 / FUN_10036310: the surface's footstep sound (else the gear's), and unless the foot is under water
// the foot dust: FOOT_DUST0 / FOOT_DUST1 by the surface's DustIndex in its DustColor (no surface entry or another
// index: none), FOOT_RAIN / FOOT_SNOW while it rains / snows; at the gear's position, scaled by its radius, only
// for units over 2 m within 250 m of the camera and not at the low particle setting
void Player::footfall(float side) {
    const Surface* s = scene_->surfaceAt(pos_.x, pos_.z);
    const std::string& wav = s && !s->footstep.empty() ? s->footstep : def_.soundSteps;
    // the sound sits 2 m along the body's x axis on the stepping side (FUN_10044440 offset on the gear)
    Vec3 at = pos_ + Mat4::rotY(yaw_).transformDir(Vec3(2.f * side, 0, 0));
    if (audio && !wav.empty()) audio->play(wav, 0.55f * soundVolume, &at, 250.f);
    if (!particles || particles->underWater(pos_) || particles->detail() == 0) return;
    float radius = scene_->objectRadius(object_);
    Vec3 d = pos_ - particles->viewer();
    if (radius <= 2.f || dot(d, d) >= 62500.f) return;
    int w = particles->weatherType();
    if (w == 1) particles->spawn("FOOT_RAIN", pos_, radius);
    else if (w == 2) particles->spawn("FOOT_SNOW", pos_, radius);
    else if (s && (s->dustIndex == 0 || s->dustIndex == 1)) particles->spawn(s->dustIndex ? "FOOT_DUST1" : "FOOT_DUST0", pos_, radius, Vec3(0, 1, 0), 1.f, s->dustColor);
}

void Player::chaseCamera(Vec3& eye, Vec3& target, float pitch, float orbit, float zoom) const {
    // FUN_1007c9f0: the view is (0, zoom x radius x sin pitch, zoom x radius x cos pitch) from the sphere centre (the
    // body's +z is behind it), turned by the orbit and then by the body's own frame; no smoothing
    float r = boundR_ > 0 ? boundR_ : 5.f;
    float a = yaw_ + orbit;
    Vec3 c = boundR_ > 0 ? Mat4::rotY(yaw_).transformDir(boundC_) : Vec3(0, 4.5f, 0);
    target = pos_ + c;
    float d = zoom * r;
    eye = target + Vec3(std::sin(a), 0, std::cos(a)) * (d * std::cos(pitch)) + Vec3(0, d * std::sin(pitch), 0);
}

void Player::cockpitCamera(Vec3& eye, Vec3& target, float pitch, float orbit) const {
    float a = yaw_ + orbit;
    Vec3 fwd(-std::sin(a), 0, -std::cos(a));
    eye = pos_ + Vec3(0, eyeHeight(), 0) + Vec3(-std::sin(yaw_), 0, -std::cos(yaw_)) * 0.8f;
    // pitch > 0 looks down in the chase camera convention; keep the same sign here
    target = eye + fwd * std::cos(pitch) + Vec3(0, -std::sin(pitch), 0);
}

}  // namespace hg2
