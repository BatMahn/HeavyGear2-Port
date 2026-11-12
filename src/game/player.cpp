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
        bool fromCycle = loop_ && (state_ == sWalk_ || state_ == sRun_);
        // stand <-> walk: the stance and the stride differ by a metre at the feet; cross-fade (port: 0.2 s)
        if (t.target == sWalk_ || t.target == sIdle_ || t.target == sRun_) fadeSet_ = set_, fadeFrame_ = frame_, fadeLen_ = 0, fadeWeight_ = 1.f;
        int len = std::max(1, loop_ ? scene_->animCycle(object_, set_) : scene_->animLength(object_, set_));
        float phase = loop_ ? frame_ / float(len) : 0.f;
        state_ = t.target;
        set_ = t.events.empty() ? t.target : t.events[0].id;
        loop_ = ecy_.states[size_t(state_)].type != 0;
        // cycles hand over at the same phase (walk <-> run); a kind-1 transition from elsewhere starts the cycle
        // at its event's phase (stand -> walk: 0.3); one-shots start at the beginning
        if (loop_ && !fromCycle && t.kind == 1 && !t.events.empty()) phase = t.events[0].b;
        frame_ = loop_ ? phase * float(std::max(1, scene_->animCycle(object_, set_))) : 0.f;
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
        // locomotion within the current posture. Standing up, the original picks the cycle from the body's speed
        // (FUN_10064700 / FUN_10064d20's caller): trigger 1 (stop) once |v| is under 1 % of the top speed with no
        // throttle, else 2 (walk) up to the walk speed (+0x10b4) and 3 (run) above it. Walk <-> run waits for one
        // of the transition's sync points (see advanceCycle).
        const float vmaxNow = sms_ ? run * 1.5f : run;
        const float vabs = std::fabs(speed_);
        const bool rolling = moving || vabs > 0.01f * vmaxNow;
        wantCycle_ = -1;
        if (!sms_ && settled) {
            if (state_ == sIdle_ && rolling) fire(kTrigWalk);
            else if ((state_ == sWalk_ || state_ == sRun_) && !rolling) fire(kTrigStop);
            else if (state_ == sWalk_ && sRun_ >= 0 && speed_ > walk * speedScale + 0.01f) wantCycle_ = kTrigRun;
            else if (state_ == sRun_ && (speed_ <= walk * speedScale || speed_ < 0)) wantCycle_ = kTrigWalk;
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
    } else if (!ecy || state_ == sWalk_ || state_ == sRun_ || state_ == sIdle_) {
        target = thr > 0 ? thr * (in.run ? run : walk) : thr * walk * 0.6f;
        targetStrafe = str * walk * 0.6f;
    } else if (state_ == sKneelWalk_) {
        target = thr * walk * 0.35f;
    } else if (state_ == sCrawl_) {
        target = thr * walk * 0.2f;
    }
    target *= speedScale, targetStrafe *= speedScale;
    float turnRate = (60.f + 6.f * float(def_.maneuverability)) * kDeg * (sms_ ? 0.6f : 1.f);
    if (postureCode() == 0) turnRate *= 0.4f;
    turnRate *= turnScale;
    if (!air_) yaw_ += in.turn * turnRate * dt;
    // models face -Z; world = local * rotY(yaw). With the left-handed view, screen right is (-cos, 0, sin).
    Vec3 fwd(-std::sin(yaw_), 0, -std::cos(yaw_)), right(-std::cos(yaw_), 0, std::sin(yaw_));
    if (!air_) drive(dt, target, targetStrafe, std::fabs(thr) > 0.05f, std::fabs(in.turn) > 0.05f, ecy, fwd, right);

    // animation: advance the set, and on the ground let a locomotion cycle's root motion carry the body
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
    prevPhase = frame_ / float(len);
    int shown = set_;
    Vec3 travel;  // model-space root motion this step
    float travelShare = 0;  // the part of it that is the body's forward motion
    if (sms_) {
        frame_ = std::fmod(frame_, float(len));
    } else if (cycle) {
        // FUN_10064d20: the cycle plays at speed / stride cycles a second (FUN_10055290 with FUN_100344e0's inverse
        // stride), and the body moves by the root's travel over the step, not by velocity × dt
        float v = std::max(std::fabs(speed_), std::fabs(strafe_));
        travelShare = v > 1e-3f ? std::fabs(speed_) / v : 0.f;
        travel = advanceCycle(dt, std::copysign(v, speed_ < -0.1f ? -1.f : 1.f));
        shown = set_, len = std::max(1, scene_->animCycle(object_, set_));
    } else if (ecy && state_ == sIdle_ && loop_ && std::fabs(in.turn) > 0.1f && sWalk_ >= 0) {
        // turning on the spot: step with the walk cycle at a slow pace
        shown = 1, rate = 18.f * std::fabs(in.turn), len = std::max(1, scene_->animCycle(object_, shown));
        cycle = true;
        if (idleSet_ != shown) frame_ = 0, idleSet_ = shown;
        frame_ = std::fmod(frame_ + rate * dt + float(len), float(len));
    } else {
        if (idleSet_ != set_ && state_ == sIdle_) frame_ = 0, idleSet_ = set_;
        if (loop_) frame_ = std::fmod(frame_ + rate * dt + float(len), float(len));
        else frame_ = std::min(frame_ + rate * dt, float(len - 1));
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
            if (particles) particles->spawn("FOOT_LANDDUST0", pos_, 0.6f);
        }
    } else if (ground < pos_.y - 1.5f) {
        air_ = true, vy_ = 0;  // walked off an edge
    } else {
        pos_.y = ground;
    }

    scene_->setObjectPose(object_, pos_, yaw_, shown, frame_);
    if (fadeSet_ >= 0 && shown == set_ && fadeWeight_ > 0) {
        fadeWeight_ -= dt / 0.2f;
        // walk <-> run: the faded cycle keeps step with the new one (they switched at the same phase); stand <-> walk:
        // the old pose holds
        float ph = frame_ / float(std::max(1, scene_->animCycle(object_, set_)));
        scene_->setObjectFade(object_, fadeSet_, fadeLen_ > 0 ? ph * float(fadeLen_) : fadeFrame_, std::max(0.f, fadeWeight_));
        if (fadeWeight_ <= 0) fadeSet_ = -1;
    } else if (fadeSet_ >= 0) {
        fadeSet_ = -1;
        scene_->setObjectFade(object_, -1, 0, 0);
    }
    if (getenv("HG2_FOOTDBG") && object_ == atoi(getenv("HG2_FOOTDBG"))) {
        Vec3 l, r;
        const char* fl = getenv("HG2_FOOTL") ? getenv("HG2_FOOTL") : "frm-lld"; const char* frr = getenv("HG2_FOOTR") ? getenv("HG2_FOOTR") : "frm-rld";
        if (scene_->frameBottom(object_, fl, l) && scene_->frameBottom(object_, frr, r))
            SDL_Log("foot %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f %.3f speed %.2f set %d frame %.1f side %.2f slope %.1f yaw %.4f", pos_.x, pos_.y, pos_.z, l.x, l.y, l.z,
                    r.x, r.y, r.z, speed_, shown, frame_, strafe_, std::acos(std::min(1.f, scene_->groundNormal(pos_.x, pos_.z).y)) / kDeg, yaw_);
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

void Player::drive(float dt, float target, float targetStrafe, bool throttle, bool turning, bool ecy, const Vec3& fwd,
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
    const float g = 20.f, mu = 2.f;         // DAT_10163ae8, +0xff0
    float vmax = (sms_ ? def_.maxRunSpeed * 1.5f : def_.maxRunSpeed) * std::max(0.1f, speedScale);  // +0x270
    vmax = std::max(1.f, vmax);
    // FUN_1003cc30 (vtable +0x268): with the ground normal n (the triangle under the body's centre, FUN_1003db20 /
    // the terrain tile's plane query) in the gear's frame, n_h |n_h| × k × g on each horizontal axis,
    // k = min(3 / n_y^2, 6) (skates min(5 / n_y^2, 9)). On ground flatter than n_y 0.8 (36.9°) the sideways part is
    // dropped, and the forward part too with no throttle. It is added after the traction cap below, so with the drive
    // at most 20 a gear can't climb past ~30° (60 tan^2 = 20), and on faces steeper than 36.9° the slope beats the
    // 40 of traction and the gear slides until the ground levels out.
    float sF = 0, sR = 0;
    if (ecy) {
        Vec3 n = scene_->groundNormal(pos_.x, pos_.z);
        float ny = n.y < 1e-5f ? 1.f : n.y;
        float k = sms_ ? std::min(5.f / (ny * ny), 9.f) : std::min(3.f / (ny * ny), 6.f);
        float nf = dot(n, fwd), nr = dot(n, right);
        sF = nf * std::fabs(nf) * k * g, sR = nr * std::fabs(nr) * k * g;
        if (ny > 0.8f) {
            sR = 0;
            if (!throttle) sF = 0;
        }
    }
    // FUN_1003c0b0: forward drive = drivek × throttle, throttle in -1..1. The port's input asks for a speed: the
    // throttle that holds it on the flat (drive = resistance: throttle = r^2 = (target / vmax)^2), plus what a pilot
    // adds to hold it against the slope and to close the gap, never past full throttle.
    float u = target / vmax;
    float aF = 0;
    if (throttle) {
        float t = u * std::fabs(u) - sF / drivek + (target - speed_) / vmax;
        aF = drivek * std::clamp(t, -1.f, 1.f);
    }
    // FUN_1003c5f0: forward resistance on r = v / vmax, r |r| plus 2 r with no throttle; r × 10 while the body isn't
    // in a moving state (+0x124a clear: stand, crouching), so a standing gear stops dead. Past r = 2 the velocity
    // is scaled back by 1 / r. It never reverses the speed within a step.
    float r = speed_ / vmax;
    if (ecy && (state_ == sIdle_ || state_ == sKneel_ || state_ == sProne_)) r *= 10.f;
    if (r > 2.f) speed_ /= r, strafe_ /= r;
    float drag = dragk * (r * std::fabs(r) + (throttle ? 0.f : 2.f * r));
    drag = std::copysign(std::min(std::fabs(drag), std::fabs(speed_) / dt), drag);
    aF -= drag;
    // FUN_1003c750: sideways resistance sidek × v_side / vmax (v_side clamped to ±20), never reversing within a step;
    // the port's sidestep is a commanded sideways speed, so it resists the difference
    float vs = strafe_ - targetStrafe;
    float aR = -sidek * std::clamp(vs, -20.f, 20.f) / vmax;
    aR = std::copysign(std::min(std::fabs(aR), std::fabs(vs) / dt), aR);
    // FUN_1003c8f0: drive and resistance together are capped at the traction mu × g; the slope force comes after
    float mag = std::hypot(aF, aR);
    if (mag > mu * g) aF *= mu * g / mag, aR *= mu * g / mag;
    aF += sF, aR += sR;
    speed_ += aF * dt;
    strafe_ += aR * dt;
    // FUN_1003af10: with no throttle or turn and under 0.05 m/s the body is put at rest
    if (!throttle && !turning && std::fabs(targetStrafe) < 1e-3f && std::hypot(speed_, strafe_) < 0.05f) speed_ = 0, strafe_ = 0;
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
        // FUN_10064d20 sets the cycle's rate to speed × 1 / stride (FUN_100344e0): one loop per stride travelled
        float stride = cycleStride(set);
        float r;
        if (stride > 0.5f) {
            r = v * float(len) / stride;
        } else {  // no root motion: pace by the state's nominal speed
            float ref = state_ == sRun_ ? def_.maxRunSpeed : state_ == sKneelWalk_ ? def_.maxWalkSpeed * 0.35f
                      : state_ == sCrawl_ ? def_.maxWalkSpeed * 0.2f : def_.maxWalkSpeed;
            r = 30.f * v / std::max(0.5f, ref);
        }
        return std::clamp(r, -90.f, 90.f);
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
                fadeSet_ = set_, fadeLen_ = len, fadeWeight_ = 1.f;  // cross-fade the old cycle out (port: 0.2 s)
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
    // two foot falls per cycle
    for (float mark : {0.25f, 0.75f}) {
        bool crossed = prevPhase <= ph ? (prevPhase < mark && ph >= mark) : (prevPhase < mark || ph >= mark);
        if (!crossed) continue;
        if (audio && !def_.soundSteps.empty()) audio->play(def_.soundSteps, 0.55f * soundVolume, &pos_, 250.f);
        if (particles && std::fabs(speed_) > 4.f) {  // a small puff where the foot lands, not round the whole gear
            Vec3 foot = pos_;
            if (!scene_->frameBottom(object_, mark < 0.5f ? "frm-lld" : "frm-rld", foot)) foot = pos_;
            foot.y = scene_->groundHeight(foot.x, foot.z);
            particles->spawn("FOOT_DUST0", foot, 0.3f);
        }
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
