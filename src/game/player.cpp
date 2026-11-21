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
              kTrigJump = 8, kTrigJets = 9;
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
    pos_.y = scene.groundHeight(pos_.x, pos_.z);
    return true;
}

int Player::stateOf(int from, int trigger) const {
    if (from < 0 || size_t(from) >= ecy_.states.size()) return -1;
    for (auto& t : ecy_.states[size_t(from)].transitions)
        if (t.trigger == trigger && t.kind != 3 && t.target >= 0 && size_t(t.target) < ecy_.states.size()) return t.target;
    return -1;
}

void Player::resolveStates() {
    if (ecy_.states.empty()) return;
    sIdle_ = 0;
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
    state_ = sIdle_, set_ = 0, loop_ = true, rate_ = 30.f;
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
        pos_.y = scene_->groundHeight(pos_.x, pos_.z);
        scene_->setObjectPose(object_, pos_, yaw_, set_, frame_);
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
    {
        // yaw: torque (20 + 4 m) x command (+0xf50, FUN_1003c0b0), damping 10 x rate (+0x1010, FUN_1003d390) and,
        // with no turn input on the ground, another 3 x rate (FUN_1003c750: 30 x -0.1)
        float sdt = std::min(dt, 0.1f);
        float a = (20.f + 4.f * m) * turnCmd - 10.f * yawRate_;
        if (std::fabs(turnCmd) < 0.05f && !air_) a -= 3.f * yawRate_;
        yawRate_ += a * sdt;
        yaw_ += yawRate_ * dt;
    }
    // the sidestep is a commanded sideways speed (not decoded: the original turns the legs, +0x10e8)
    float targetStrafe = str * walkLimit_ * 0.6f * speedScale;
    if (!air_) drive(dt, thr, targetStrafe, std::fabs(turnCmd) > 1e-5f, ecy, fwd, right);
    // jump thrust (FUN_1003af10 while +0x1070 > 0): 3.5 g up (+0x1060) and 0.2 of it forward, in the body's frame
    if (thrust_ > 0) {
        float t = std::min(dt, thrust_);
        thrust_ -= dt;
        vy_ += 3.5f * kGravity * t;
        speed_ += 0.7f * kGravity * t;
        air_ = true;
    }

    // animation: advance the set, and on the ground let a locomotion cycle's root motion carry the body
    float prevPhase = 0;
    if (!ecy) {  // no state machine: plain stand / walk / run sets
        int want = std::fabs(speed_) < 0.3f ? 0 : (speed_ > walkLimit_ ? 2 : 1);
        if (want != set_) set_ = want, frame_ = 0;
        loop_ = true;
    }
    len = std::max(1, loop_ ? scene_->animCycle(object_, set_) : scene_->animLength(object_, set_));
    bool cycle = !ecy ? set_ != 0
                      : loop_ && !skating() && !air_ && state_ != sIdle_ && state_ != sKneel_ && state_ != sProne_;
    prevPhase = frame_ / float(len);
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
    Vec3 before = pos_;
    if (scene_->collide(pos_, 2.2f, 7.f, object_)) {
        // lose the speed that went into the obstacle
        Vec3 moved = pos_ - before;
        if (length(moved) > 0.01f && dot(normalize(moved), fwd) < -0.5f) speed_ *= 0.5f;
    }
    // vertical (FUN_1003cc30): the ground is the terrain under the body's centre (or an object top); gravity acts
    // all the time and the contact stops the fall. The body counts as grounded until it is more than 2 m above the
    // ground (the player's gear: 0.5 m).
    float ground = scene_->supportHeight(pos_, air_ ? 0.3f : 1.2f, object_);
    vy_ -= kGravity * gravityScale * dt;
    pos_.y += vy_ * dt;
    if (pos_.y <= ground) {
        bool landed = air_;
        pos_.y = ground, vy_ = std::max(0.f, vy_), air_ = false;
        if (landed) {
            footfall();  // landing: the surface's footstep sound and dust (FUN_10067570)
        }
    } else if (pos_.y - ground > (pilot ? 0.5f : 2.f)) {
        air_ = true;  // walked off an edge
    }

    scene_->setObjectPose(object_, pos_, yaw_, shown, frame_);
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
    scene_->setObjectTwist(object_, twist);
    if (cycle) sounds(prevPhase);
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
    const float drivek = 20.f + 2.f * m;    // +0xf58: drive per unit throttle
    const float dragk = 20.f + 2.f * m;     // +0xf60: forward resistance
    const float sidek = 400.f + 30.f * m;   // +0xf68: sideways resistance
    const float g = kGravity, mu = 2.f;     // DAT_10163ae8, +0xff0
    const float vm = vmax();                // +0x270
    const bool hasThrottle = std::fabs(throttle) >= 0.05f;
    // FUN_1003cc30 (vtable +0x268), the player's gear only (+0x1f0; AI bodies get none): with the ground normal n
    // (the triangle under the body's centre) in the gear's frame, n_h |n_h| x k x g on each horizontal axis,
    // k = min(3 / n_y^2, 6) (skates min(5 / n_y^2, 9)). On ground flatter than n_y 0.8 (36.9 deg) the sideways part is
    // dropped, and the forward part too with no throttle. It is added after the traction cap below, so with the drive
    // at most 20 a gear can't climb past ~30 deg (60 tan^2 = 20), and on faces steeper than 36.9 deg the slope beats
    // the 40 of traction and the gear slides until the ground levels out.
    float sF = 0, sR = 0;
    if (ecy && pilot) {
        Vec3 n = scene_->groundNormal(pos_.x, pos_.z);
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

void Player::sounds(float prevPhase) {
    int len = std::max(1, scene_->animCycle(object_, set_));
    float ph = frame_ / float(len);
    // two foot falls per cycle (the original times them from each foot's contact window in the animation,
    // FUN_10067570; the port keeps fixed marks)
    for (float mark : {0.25f, 0.75f}) {
        bool crossed = prevPhase <= ph ? (prevPhase < mark && ph >= mark) : (prevPhase < mark || ph >= mark);
        if (!crossed) continue;
        footfall();
    }
}

// FUN_10067570 / FUN_10036310: the surface's footstep sound (else the gear's), and unless the foot is under water
// the foot dust: FOOT_DUST0 / FOOT_DUST1 by the surface's DustIndex in its DustColor (no surface entry or another
// index: none), FOOT_RAIN / FOOT_SNOW while it rains / snows; at the gear's position, scaled by its radius, only
// for units over 2 m within 250 m of the camera and not at the low particle setting
void Player::footfall() {
    const Surface* s = scene_->surfaceAt(pos_.x, pos_.z);
    const std::string& wav = s && !s->footstep.empty() ? s->footstep : def_.soundSteps;
    if (audio && !wav.empty()) audio->play(wav, 0.55f * soundVolume, &pos_, 250.f);
    if (!particles || particles->underWater(pos_) || particles->detail() == 0) return;
    float radius = scene_->objectRadius(object_);
    Vec3 d = pos_ - particles->viewer();
    if (radius <= 2.f || dot(d, d) >= 62500.f) return;
    int w = particles->weatherType();
    if (w == 1) particles->spawn("FOOT_RAIN", pos_, radius);
    else if (w == 2) particles->spawn("FOOT_SNOW", pos_, radius);
    else if (s && (s->dustIndex == 0 || s->dustIndex == 1)) particles->spawn(s->dustIndex ? "FOOT_DUST1" : "FOOT_DUST0", pos_, radius, Vec3(0, 1, 0), 1.f, s->dustColor);
}

void Player::chaseCamera(Vec3& eye, Vec3& target, float pitch, float orbit, float dist) const {
    float a = yaw_ + orbit;
    Vec3 back(std::sin(a), 0, std::cos(a));
    target = pos_ + Vec3(0, 4.5f, 0);
    eye = target + back * (dist * std::cos(pitch)) + Vec3(0, dist * std::sin(pitch), 0);
    float g = scene_->groundHeight(eye.x, eye.z) + 1.5f;
    if (eye.y < g) eye.y = g;
}

void Player::cockpitCamera(Vec3& eye, Vec3& target, float pitch, float orbit) const {
    float a = yaw_ + orbit;
    Vec3 fwd(-std::sin(a), 0, -std::cos(a));
    eye = pos_ + Vec3(0, eyeHeight(), 0) + Vec3(-std::sin(yaw_), 0, -std::cos(yaw_)) * 0.8f;
    // pitch > 0 looks down in the chase camera convention; keep the same sign here
    target = eye + fwd * std::cos(pitch) + Vec3(0, -std::sin(pitch), 0);
}

}  // namespace hg2
