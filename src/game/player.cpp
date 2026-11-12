#include "player.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>

#include "../audio/audio.h"
#include "particles.h"

namespace hg2 {

namespace {
constexpr float kDeg = 3.14159265358979f / 180.f;
// ECY triggers (hg2sim.dll state machine inputs), identified from the gear .ECY files:
// 1 stop, 2 walk, 3 run, 4 jump (take-off / airborne / landing chain), 6 posture down, 7 posture up.
constexpr int kTrigStop = 1, kTrigWalk = 2, kTrigRun = 3, kTrigJump = 4, kTrigDown = 6, kTrigUp = 7;
constexpr float kGravity = 16.f, kJumpSpeed = 10.f;
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
    if (def_.maxWalkSpeed <= 0) def_.maxWalkSpeed = 7;
    if (def_.maxRunSpeed <= 0) def_.maxRunSpeed = def_.maxWalkSpeed * 2.2f;
    resolveStates();
    if (o.userHeader)
        SDL_Log("player gear: %s walk %.1f run %.1f, %zu animation states (kneel %d prone %d jump %d)",
                def_.geometry.empty() ? "?" : def_.geometry[0].c_str(), def_.maxWalkSpeed, def_.maxRunSpeed,
                ecy_.states.size(), sKneel_, sProne_, sJump_);
    pos_.y = scene.groundHeight(pos_.x, pos_.z);
    return true;
}

int Player::stateOf(int from, int trigger) const {
    if (from < 0 || size_t(from) >= ecy_.states.size()) return -1;
    for (auto& t : ecy_.states[size_t(from)].transitions)
        if (t.trigger == trigger && t.target >= 0 && size_t(t.target) < ecy_.states.size()) return t.target;
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
    sJump_ = stateOf(sIdle_, kTrigJump);
    sAir_ = stateOf(sJump_, kTrigJump);
    sLand_ = stateOf(sAir_, kTrigJump);
    state_ = sIdle_, set_ = 0, loop_ = true;
}

bool Player::fire(int trigger) {
    if (size_t(state_) >= ecy_.states.size()) return false;
    for (auto& t : ecy_.states[size_t(state_)].transitions) {
        if (t.trigger != trigger || t.target < 0 || size_t(t.target) >= ecy_.states.size()) continue;
        int len = std::max(1, scene_->animLength(object_, set_));
        float phase = loop_ ? frame_ / float(len) : 0.f;
        state_ = t.target;
        set_ = t.events.empty() ? t.target : t.events[0].id;
        loop_ = ecy_.states[size_t(state_)].type != 0;
        // cycles hand over at the same phase (walk <-> run); one-shots start at the beginning
        frame_ = loop_ ? phase * float(std::max(1, scene_->animLength(object_, set_))) : 0.f;
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

void Player::revive() {
    dead_ = false, speed_ = 0, strafe_ = 0, vy_ = 0, air_ = false, sms_ = false;
    reactSet_ = -1;
    if (scene_ && object_ >= 0) scene_->setObjectOverlay(object_, -1, 0, "");
    resolveStates();
    frame_ = 0;
}

void Player::die() {
    if (dead_ || object_ < 0) return;
    dead_ = true, speed_ = 0, strafe_ = 0, sms_ = false;
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
    if (sms_) return 3;
    if (state_ >= 0 && (state_ == sProne_ || state_ == sCrawl_)) return 0;
    if (state_ >= 0 && (state_ == sKneel_ || state_ == sKneelWalk_)) return 2;
    return 1;
}

float Player::eyeHeight() const {
    int p = postureCode();
    return p == 0 ? 1.6f : p == 2 ? 3.4f : 5.0f;
}

void Player::update(float dt, const PlayerInput& in) {
    if (object_ < 0) return;
    if (dead_) {  // play the fall once and stay down
        int len = std::max(1, scene_->animLength(object_, set_));
        frame_ = std::min(frame_ + 30.f * dt, float(len - 1));
        pos_.y = scene_->groundHeight(pos_.x, pos_.z);
        scene_->setObjectPose(object_, pos_, yaw_, set_, frame_);
        scene_->setObjectTwist(object_, 0);
        return;
    }
    const bool ecy = canWalk();
    const float walk = def_.maxWalkSpeed, run = def_.maxRunSpeed;
    float thr = lockForward_ ? 0.f : in.throttle, str = lockForward_ ? 0.f : in.strafe;
    bool moving = std::fabs(thr) > 0.05f || std::fabs(str) > 0.05f;
    int len = std::max(1, scene_->animLength(object_, set_));
    bool done = !loop_ && frame_ >= float(len - 1);
    bool settled = loop_ || done;
    bool standing = state_ == sIdle_ || state_ == sWalk_ || state_ == sRun_;

    if (ecy && !air_) {
        // posture, jump and skates (SMS) inputs
        if (in.toggleSkates && standing && std::fabs(speed_) < 1.5f) sms_ = !sms_;
        bool down = in.down, up = in.up;
        if (wantPosture_ >= 0 && !moving && settled && !sms_) {  // scripted posture (SetStandingMode)
            int cur = postureCode(), rank[3] = {2, 0, 1};  // prone 2, stand 0, kneel 1 steps below standing
            if (rank[wantPosture_ % 3] > rank[cur % 3]) down = true;
            else if (rank[wantPosture_ % 3] < rank[cur % 3]) up = true;
        }
        if (!sms_ && settled) {
            if (down && (state_ == sIdle_ || state_ == sKneel_)) fire(kTrigDown);
            else if (up && (state_ == sKneel_ || state_ == sProne_)) fire(kTrigUp);
            else if (in.jump && standing && sJump_ >= 0) fire(kTrigJump);
        }
        len = std::max(1, scene_->animLength(object_, set_));
        done = !loop_ && frame_ >= float(len - 1);
        settled = loop_ || done;
        // locomotion within the current posture
        if (!sms_ && settled) {
            if (state_ == sIdle_ && moving) fire(kTrigWalk);
            else if (state_ == sWalk_ && !moving) fire(kTrigStop);
            else if (state_ == sWalk_ && in.run && thr > 0 && sRun_ >= 0) fire(kTrigRun);
            else if (state_ == sRun_ && !moving) fire(kTrigStop);
            else if (state_ == sRun_ && (!in.run || thr <= 0)) fire(kTrigWalk);
            else if (state_ == sKneel_ && moving && sKneelWalk_ >= 0) fire(kTrigWalk);
            else if (state_ == sKneelWalk_ && !moving) fire(kTrigStop);
            else if (state_ == sProne_ && moving && sCrawl_ >= 0) fire(kTrigWalk);
            else if (state_ == sCrawl_ && !moving) fire(kTrigStop);
        }
        // jump chain: take-off -> airborne -> landing -> idle
        if (state_ == sJump_ && sJump_ >= 0 && !loop_ && frame_ >= float(std::max(1, scene_->animLength(object_, set_)) - 1)) {
            vy_ = kJumpSpeed * (1.f + 0.35f * float(jumpJets)), air_ = true;
            if (!fire(kTrigJump)) state_ = sIdle_, set_ = 0, loop_ = true;
        } else if (state_ == sLand_ && sLand_ >= 0 && !loop_ &&
                   frame_ >= float(std::max(1, scene_->animLength(object_, set_)) - 1)) {
            state_ = sIdle_, set_ = 0, loop_ = true, frame_ = 0;
        }
    }

    // target speeds for the current state
    float target = 0, targetStrafe = 0;
    if (air_) {
        target = speed_, targetStrafe = strafe_;
    } else if (sms_) {
        target = thr * run * 1.5f;
    } else if (!ecy || state_ == sWalk_ || state_ == sRun_) {
        target = thr > 0 ? thr * (in.run ? run : walk) : thr * walk * 0.6f;
        targetStrafe = str * walk * 0.6f;
    } else if (state_ == sKneelWalk_) {
        target = thr * walk * 0.35f;
    } else if (state_ == sCrawl_) {
        target = thr * walk * 0.2f;
    }
    target *= speedScale, targetStrafe *= speedScale;
    float accel = sms_ ? 5.f : 8.f;  // m/s^2
    if (speed_ < target) speed_ = std::min(target, speed_ + accel * dt);
    else speed_ = std::max(target, speed_ - accel * 1.5f * dt);
    if (strafe_ < targetStrafe) strafe_ = std::min(targetStrafe, strafe_ + accel * dt);
    else strafe_ = std::max(targetStrafe, strafe_ - accel * 1.5f * dt);
    float turnRate = (60.f + 6.f * float(def_.maneuverability)) * kDeg * (sms_ ? 0.6f : 1.f);
    if (postureCode() == 0) turnRate *= 0.4f;
    turnRate *= turnScale;
    if (!air_) yaw_ += in.turn * turnRate * dt;
    // models face -Z; world = local * rotY(yaw). With the left-handed view, screen right is (-cos, 0, sin).
    Vec3 fwd(-std::sin(yaw_), 0, -std::cos(yaw_)), right(-std::cos(yaw_), 0, std::sin(yaw_));
    pos_ += fwd * (speed_ * dt) + right * (strafe_ * dt);
    pos_.x = std::max(1.f, std::min(4095.f, pos_.x));
    pos_.z = std::max(1.f, std::min(4095.f, pos_.z));
    Vec3 before = pos_;
    if (scene_->collide(pos_, 2.2f, 7.f, object_)) {
        // lose the speed that went into the obstacle
        Vec3 moved = pos_ - before;
        if (length(moved) > 0.01f && dot(normalize(moved), fwd) < -0.5f) speed_ *= 0.5f;
    }
    // vertical: terrain or object tops (platforms, blocks), gravity while airborne
    // stand on the highest ground under the footprint (the feet), not just the centre: on a slope the uphill foot
    // would otherwise sink into the hill
    float ground = scene_->supportHeight(pos_, air_ ? 0.3f : 1.2f, object_);
    {
        const float fr = footprint_;
        for (const Vec3& o : {fwd * fr, fwd * -fr, right * fr, right * -fr}) {
            float h = scene_->groundHeight(pos_.x + o.x, pos_.z + o.z);
            ground = std::max(ground, std::min(h, pos_.y + 2.5f));  // not up a cliff face
        }
    }
    if (air_) {
        vy_ -= kGravity * gravityScale * dt;
        pos_.y += vy_ * dt;
        if (pos_.y <= ground && vy_ <= 0) {
            pos_.y = ground, vy_ = 0, air_ = false;
            if (state_ == sAir_ && sAir_ >= 0) fire(kTrigJump);
            if (audio && !def_.soundSteps.empty()) audio->play(def_.soundSteps, 0.8f * soundVolume, &pos_, 250.f);
            if (particles) particles->spawn("FOOT_LANDDUST0", pos_, 1.f);
        }
    } else if (ground < pos_.y - 1.5f) {
        air_ = true, vy_ = 0;  // walked off an edge
    } else {
        pos_.y = ground;
    }

    // animation
    float prevPhase = 0;
    if (!ecy) {  // no state machine: plain stand / walk / run sets
        int want = std::fabs(speed_) < 0.3f ? 0 : (speed_ > walk * 1.15f ? 2 : 1);
        if (want != set_) set_ = want, frame_ = 0;
        loop_ = true;
    }
    len = std::max(1, loop_ ? scene_->animCycle(object_, set_) : scene_->animLength(object_, set_));
    float rate = 30.f;  // XB keys are 30 per second
    bool cycle = !ecy ? set_ != 0
                      : loop_ && !sms_ && !air_ && state_ != sIdle_ && state_ != sKneel_ && state_ != sProne_;
    if (sms_) {
        rate = 0;
    } else if (cycle) {
        float ref = (ecy ? state_ == sRun_ : set_ == 2) ? run : state_ == sKneelWalk_ ? walk * 0.35f : state_ == sCrawl_ ? walk * 0.2f : walk;
        float v = std::max(std::fabs(speed_), std::fabs(strafe_));
        // the cycle's own pace: its root travel (stride) over its length; play it so the feet keep up with the ground
        float stride = scene_->animStride(object_, set_);
        if (stride > 0.5f) ref = stride / (float(len) / 30.f);
        rate *= std::clamp(v / std::max(0.5f, ref), 0.3f, 3.f);
        if (speed_ < -0.1f) rate = -rate;
    }
    prevPhase = frame_ / float(len);
    int shown = set_;
    if (ecy && state_ == sIdle_ && loop_ && !sms_ && std::fabs(in.turn) > 0.1f && sWalk_ >= 0) {
        // turning on the spot: step with the walk cycle at a slow pace
        shown = 1, rate = 18.f * std::fabs(in.turn), len = std::max(1, scene_->animCycle(object_, shown));
        cycle = true;
        if (idleSet_ != shown) frame_ = 0, idleSet_ = shown;
    } else if (idleSet_ != set_ && state_ == sIdle_) {
        frame_ = 0, idleSet_ = set_;
    }
    if (loop_) frame_ = std::fmod(frame_ + rate * dt + float(len), float(len));
    else frame_ = std::min(frame_ + rate * dt, float(len - 1));
    scene_->setObjectPose(object_, pos_, yaw_, shown, frame_);
    if (getenv("HG2_FOOTDBG") && object_ == atoi(getenv("HG2_FOOTDBG"))) {
        Vec3 l, r;
        const char* fl = getenv("HG2_FOOTL") ? getenv("HG2_FOOTL") : "frm-lld"; const char* frr = getenv("HG2_FOOTR") ? getenv("HG2_FOOTR") : "frm-rld";
        if (scene_->frameBottom(object_, fl, l) && scene_->frameBottom(object_, frr, r))
            SDL_Log("foot %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f speed %.2f set %d frame %.1f", pos_.x, pos_.y, pos_.z, l.x, l.y, l.z,
                    r.x, r.y, r.z, speed_, shown, frame_);
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
        float v = std::min(1.f, std::fabs(speed_) / std::max(1.f, run));
        audio->setLoop(motor_, soundVolume * (0.12f + 0.3f * v), &pos_, 400.f);
    }
}

void Player::sounds(float prevPhase) {
    int len = std::max(1, scene_->animLength(object_, set_));
    float ph = frame_ / float(len);
    // two foot falls per cycle
    for (float mark : {0.25f, 0.75f}) {
        bool crossed = prevPhase <= ph ? (prevPhase < mark && ph >= mark) : (prevPhase < mark || ph >= mark);
        if (!crossed) continue;
        if (audio && !def_.soundSteps.empty()) audio->play(def_.soundSteps, 0.55f * soundVolume, &pos_, 250.f);
        if (particles && std::fabs(speed_) > 4.f) particles->spawn("FOOT_DUST0", pos_, 0.6f);
    }
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
