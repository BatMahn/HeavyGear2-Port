#include "particles.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <unordered_map>

#include "../assets/assets.h"

namespace hg2 {

namespace {
float f32(const Bytes& b, size_t off) {
    float v = 0;
    if (off + 4 <= b.size()) memcpy(&v, b.data() + off, 4);
    return std::isfinite(v) ? v : 0.f;
}
uint32_t u32(const Bytes& b, size_t off) {
    uint32_t v = 0;
    if (off + 4 <= b.size()) memcpy(&v, b.data() + off, 4);
    return v;
}
std::string cstr(const Bytes& b, size_t off, size_t max) {
    std::string s;
    for (size_t i = off; i < b.size() && i < off + max && b[i]; i++) s += char(b[i]);
    return s;
}
constexpr size_t kMaxParticles = 8000;  // memory guard only; the emitters' own counts keep the totals far below
constexpr float kStep = 0.03333f;       // FUN_10124ee0: the particle system steps in 1/30 s, at most 5 a frame
// Weather tiles: the original draws its drop field once for every visible 32-unit terrain tile (FUN_10096fe0)
constexpr int kTileReach = 12;
}  // namespace

void Particles::init(const FileSystem& fs, Renderer& r, int detail) {
    fs_ = &fs, r_ = &r;
    detail_ = detail;
    // FUN_10126f70 etc.: "particle system\low\", "\med\" or "\hi\" by the particle setting
    dir_ = std::string("particle system/") + (detail == 0 ? "low" : detail == 2 ? "med" : "hi") + "/";
}

bool Particles::read(const std::string& file, Bytes& out) const { return fs_ && fs_->read(dir_ + file, out); }

int Particles::texture(const std::string& name) {
    std::string tk = upper(name);
    auto ti = textures_.find(tk);
    if (ti != textures_.end()) return ti->second;
    int id = -1;
    Bytes tb;
    Image img;
    if (fs_->read(name, tb) && decodeImage(tb, name, img)) id = r_->createTexture(img);
    textures_[tk] = id;
    return id;
}

const ParticleType* Particles::type(const std::string& name) {
    std::string k = upper(name);
    auto it = types_.find(k);
    if (it != types_.end()) return it->second.get();
    std::unique_ptr<ParticleType> t;
    Bytes b;
    if (read(name + ".ptl", b) && b.size() >= 0x1aa0) {  // read into a zeroed 0x1ab4-byte record
        t = std::make_unique<ParticleType>();
        t->name = cstr(b, 0, 256);
        t->life = f32(b, 0x100), t->lifeVar = f32(b, 0x104);
        t->flags = u32(b, 0x108);
        t->drag = f32(b, 0x110);
        t->gravity = f32(b, 0x114) * 9.8f;  // _DAT_10163b08
        t->texture = cstr(b, 0x118, 256);
        for (int i = 0; i < 16; i++) t->uv[i][0] = f32(b, 0x21c + size_t(i) * 8), t->uv[i][1] = f32(b, 0x220 + size_t(i) * 8);
        t->cellW = f32(b, 0x29c), t->cellH = f32(b, 0x2a0);
        t->frames = std::max(1, std::min(16, int(u32(b, 0x2a4))));
        t->mode = int(u32(b, 0x2a8));
        if (t->mode == 0 && t->frames > 1) t->mode = 11;  // FUN_10126a30
        if (upper(name) == "MUZZLE_FLASH") t->mode = 10;
        t->wind = (u32(b, 0x1aac) & 0x80000000u) != 0;
        t->anim = f32(b, 0x1ab0);
        for (int i = 0; i < 256; i++) {
            for (int c = 0; c < 4; c++) t->rgba[i][c] = std::max(0.f, std::min(1.f, f32(b, 0x2ac + size_t(c) * 0x400 + size_t(i) * 4)));
            t->size[i] = f32(b, 0x16ac + size_t(i) * 4) * 0.5f;
        }
        if (!t->texture.empty()) t->tex = texture(t->texture);
    }
    auto* raw = t.get();
    types_[k] = std::move(t);
    return raw;
}

const Emitter* Particles::emitter(const std::string& name) {
    std::string k = upper(name);
    auto it = emitters_.find(k);
    if (it != emitters_.end()) return it->second.get();
    std::unique_ptr<Emitter> e;
    Bytes b;
    if (read(name + ".emt", b) && b.size() >= 0x128) {
        e = std::make_unique<Emitter>();
        e->name = cstr(b, 0, 16);
        e->flags = u32(b, 0x100);
        e->shape = int(u32(b, 0x104));
        e->drag = f32(b, 0x108);
        e->duration = f32(b, 0x10c), e->durationVar = f32(b, 0x110);
        e->count = int(u32(b, 0x114));
        e->speed = f32(b, 0x118), e->speedVar = f32(b, 0x11c);
        e->cone = f32(b, 0x120), e->radius = f32(b, 0x124);
        for (int i = 0; i < 256; i++) e->rate[i] = f32(b, 0x128 + size_t(i) * 4), e->speedCurve[i] = f32(b, 0x528 + size_t(i) * 4);
    }
    auto* raw = e.get();
    emitters_[k] = std::move(e);
    return raw;
}

const Effect* Particles::effect(const std::string& name) {
    std::string k = upper(name);
    auto it = effects_.find(k);
    if (it != effects_.end()) return it->second.get();
    // effects are looked up by .ped name only (FUN_10124480 searches the loaded .ped list); a name without one
    // makes nothing
    auto fx = std::make_unique<Effect>();
    Bytes b;
    if (read(name + ".ped", b) && b.size() >= 0x104) {
        uint32_t n = std::min<uint32_t>(u32(b, 0x100), 16);
        for (uint32_t i = 0; i < n; i++) {
            size_t o = 0x104 + size_t(i) * 0x218;
            if (o + 0x218 > b.size()) break;
            Effect::Part p{emitter(cstr(b, o, 256)), type(cstr(b, o + 0x100, 256)), u32(b, o + 0x200), f32(b, o + 0x204),
                           f32(b, o + 0x208), f32(b, o + 0x20c), f32(b, o + 0x210), f32(b, o + 0x214)};
            if (p.emitter && p.type) fx->parts.push_back(p);  // FUN_101256a0 needs both
        }
        // the lone particles at +0x2284 (SPHERICAL, MUSHROOM, WATER_SWIRL) name types no shipped folder has
    }
    if (fx->parts.empty()) {
        fx.reset();
        SDL_Log("effect not found: %s", name.c_str());
    }
    auto* raw = fx.get();
    effects_[k] = std::move(fx);
    return raw;
}

Vec3 Particles::randomUnit() {  // the 4096-entry table of unit vectors at DAT_1027ebb0
    for (;;) {
        Vec3 v(rnd() * 2 - 1, rnd() * 2 - 1, rnd() * 2 - 1);
        float l = length(v);
        if (l > 1e-3f && l <= 1.f) return v * (1.f / l);
    }
}

// FUN_101260c0: one emitter per .ped entry, scaled by (scale +- variance) x the caller's scale; an entry with a
// velocity sets the emitter moving in a random direction inside its cone (about the effect's up).
static void frameOf(const Vec3& up, Vec3& ax, Vec3& ay, Vec3& az) {
    ay = length(up) > 1e-4f ? normalize(up) : Vec3(0, 1, 0);
    Vec3 a = std::fabs(ay.y) < 0.9f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
    ax = normalize(cross(a, ay)), az = cross(ax, ay);
}

bool Particles::spawn(const std::string& name, const Vec3& pos, float scale, const Vec3& up, float, uint32_t tint) {
    const Effect* fx = effect(name);
    if (!fx) return false;
    int h = nextHandle_++;
    for (auto& p : fx->parts) {
        Inst e;
        e.part = p;
        e.pos = e.prev = pos;
        frameOf(up, e.ax, e.ay, e.az);
        e.scale = (p.scale + (rnd() * 2 - 1) * p.scaleVar) * scale;
        if (std::fabs(p.vel) > 0.01f) {
            Vec3 d = randomUnit();
            float c = std::cos(p.cone * 0.01745329f), s = std::sin(p.cone * 0.01745329f);
            if (c <= 0) {
                if (d.y < 0) d.y = -c * d.y;
            } else {
                d.x *= s, d.z *= s, d.y = std::fabs(d.y);
                d = length(d) <= std::sqrt(0.05f) ? Vec3(0, 1, 0) : normalize(d);
            }
            float sp = (p.vel + (rnd() * 2 - 1) * p.velVar) * scale;
            e.vel = (e.ax * d.x + e.ay * d.y + e.az * d.z) * sp;
        }
        e.rate = 1.f / std::max(1e-3f, p.emitter->duration + rnd() * p.emitter->durationVar);
        e.id = nextId_++, e.handle = h, e.tint = tint;
        insts_.push_back(e);
    }
    return true;
}

int Particles::attach(const std::string& name, const Vec3& pos, float scale, uint32_t tint) {
    size_t first = insts_.size();
    if (!spawn(name, pos, scale, Vec3(0, 1, 0), 1.f, tint)) return 0;
    int h = insts_.empty() ? 0 : insts_.back().handle;
    for (size_t i = first; i < insts_.size(); i++) insts_[i].attached = insts_[i].part.attach != 0;
    return h;
}

void Particles::move(int handle, const Vec3& pos, const Vec3& dir) {
    if (!handle) return;
    for (auto& e : insts_)
        if (e.handle == handle && e.attached) {
            e.pos = pos;
            frameOf(length(dir) > 1e-4f ? normalize(dir) * -1.f : Vec3(0, 1, 0), e.ax, e.ay, e.az);
        }
}

void Particles::detach(int handle) {  // FUN_10124530: the effect's emitters end; their particles live on
    if (!handle) return;
    insts_.erase(std::remove_if(insts_.begin(), insts_.end(), [&](const Inst& e) { return e.handle == handle && e.attached; }),
                 insts_.end());
}

// FUN_10123bd0: one particle from an emitter. Speed (speed +- variance) x the speed curve x the emitter's scale; the
// direction is a random unit vector (shape 0), bent into the cone and pushed out to the radius (shape 1), or onto
// the ring (shape 2); flag 2 adds the emitter's own velocity.
void Particles::emitOne(Inst& e, const Vec3& at, float speedFactor) {
    if (parts_.size() >= kMaxParticles) return;
    const Emitter& em = *e.part.emitter;
    float speed = ((rnd() * 2 - 1) * em.speedVar + em.speed) * speedFactor;
    Vec3 d = randomUnit(), p = at;
    float c = std::cos(em.cone * 0.01745329f), s = std::sin(em.cone * 0.01745329f);
    if (em.shape == 1 || em.shape == 2) {
        if (em.shape == 1) {
            if (em.cone > 5.f) {
                if (c <= 0) {
                    if (d.y < 0) d.y = -c * d.y;
                } else {
                    d.x *= s, d.z *= s, d.y = std::fabs(d.y);
                    if (length(d) > 1e-5f) d = normalize(d);
                }
            } else {
                d = Vec3(0, 1, 0);
            }
        } else {
            d.y = std::fabs(d.y) * c;
            d = length(d) <= 1e-5f ? Vec3(0, 1, 0) : normalize(d);
        }
        d = e.ax * d.x + e.ay * d.y + e.az * d.z;
        p = p + d * (em.radius * speedFactor);
    }
    Particle q;
    q.pos = p;
    q.vel = d * speed;
    if (em.flags & 2) q.vel = q.vel + e.vel;
    const ParticleType* t = e.part.type;
    q.type = t;
    q.rate = 1.f / std::max(1e-3f, t->life + rnd() * t->lifeVar);  // FUN_10125fb0
    q.scale = e.scale;
    q.tint = e.tint;
    q.motion = int(t->flags & 3) | (t->wind ? 4 : 0);
    if (q.motion == 0 && length(q.vel) < 0.01f) q.motion = 8;  // FUN_101279c0: at rest
    q.id = nextId_++, q.link = e.last, q.emitter = e.id;
    e.last = q.id;
    e.alive++;
    parts_.push_back(q);
}

void Particles::update(float dt) {
    // FUN_10124ee0: whole 1/30 s steps (at most 5 a frame), the remainder carried
    acc_ += dt;
    int n = int(acc_ / kStep);
    float st = kStep;
    if (n > 5) st = acc_ / 5.f, n = 5;
    acc_ -= float(n) * st;
    for (int i = 0; i < n; i++) step(st);
    // the gust phase steps 64 a second, and gusts switch on and off for 0, 1, .. 15 s in turn
    phase_ = uint8_t(phase_ + uint8_t(int(dt * 64.f)));
    gustTime_ += dt;
    if (float(gustCount_ & 15) < gustTime_) gustTime_ = 0, gustOn_ = !gustOn_, gustCount_++;
    // the weather object updates once a frame (FUN_100f6e60 from the world update)
    stepWeather(dt);
}

void Particles::step(float dt) {
    // emitters (FUN_10123600)
    {
        std::unordered_map<int, int> alive;
        for (auto& p : parts_) alive[p.emitter]++;
        for (auto& e : insts_) e.alive = alive[e.id];
    }
    for (size_t i = 0; i < insts_.size();) {
        Inst& e = insts_[i];
        const Emitter& em = *e.part.emitter;
        e.t += dt * e.rate;
        if (e.t >= 1.f) {
            if (!(em.flags & 0x10)) {
                insts_.erase(insts_.begin() + long(i));
                continue;
            }
            e.t -= std::floor(e.t), e.prevT = 0;
        }
        Vec3 before = e.pos;
        if (!e.attached && (em.flags & 0x100)) {  // a free-flying emitter: drag and gravity of its own
            Vec3 a;
            if (em.flags & 0x80) a = e.vel * -em.drag;
            if (em.flags & 0x20) a.y -= 9.8f;
            e.vel = e.vel + a * dt;
            e.pos = e.pos + e.vel * dt;
        }
        e.prev = before;
        if (e.alive < em.count) {
            // FUN_10124030: the rate curve integrated over the emission time passed since the last emission
            float a0 = e.prevT * 256.f, a1 = e.t * 256.f;
            int k0 = std::min(255, int(a0)), k1 = std::min(255, int(a1));
            float sum;
            if (k0 == k1) {
                sum = (e.t - e.prevT) * em.rate[k0];
            } else {
                sum = (float(k0 + 1) / 256.f - e.prevT) * em.rate[k0];
                for (int k = k0 + 1; k < k1; k++) sum += em.rate[k] / 256.f;
                sum += (e.t - float(k1) / 256.f) * em.rate[k1];
            }
            int cnt = int(sum);
            if (cnt > 0) {
                cnt = std::min(cnt, em.count - e.alive);
                Vec3 d = e.pos - e.prev;
                if (e.attached && e.part.type->mode == 2 && dot(d, d) < 0.1f) {
                    ++i;
                    continue;
                }
                float sf = em.speedCurve[std::min(255, int(e.t * 256.f))] * e.scale;
                Vec3 at = e.prev, stepv = d * (1.f / float(cnt));
                for (int c = 0; c < cnt; c++, at = at + stepv) emitOne(insts_[i], at, sf);
                insts_[i].prevT = insts_[i].t;
            }
        }
        ++i;
    }
    // particles (the motion table at DAT_10296ca8, FUN_10127920)
    for (auto& p : parts_) {
        p.age += dt * p.rate;
        const ParticleType& t = *p.type;
        int m = p.motion;
        if (m == 4 || m == 6 || m == 8) continue;  // FUN_10127ad0: still
        if (m == 0) {
            p.pos = p.pos + p.vel * dt;
            continue;
        }
        Vec3 dv;
        if (m == 1 || m == 3) {  // drag toward the stored air (zero after the first step)
            dv = (p.air - p.vel) * t.drag;
            p.air = Vec3();
        } else if (m == 5 || m == 7) {  // drag toward the wind, gusting along it
            dv = (p.air - p.vel) * t.drag;
            uint8_t b = uint8_t(int(dot(p.pos, wind_)) + phase_);
            float c = (gustOn_ ? 1.f : 0.f) * (float(b) - 128.f) * gust_ * 0.0078125f;
            p.air = wind_ - wind_ * c;
            // high detail (FUN_10127e90 / FUN_101286b0): within 5 m of the ground the air bounces off the slope
            if (detail_ == 1 && groundHeight && groundNormal && p.pos.y - groundHeight(p.pos.x, p.pos.z) < 5.f) {
                Vec3 ahead = p.pos + p.air * (dt * 2.f), nrm = groundNormal(ahead.x, ahead.z);
                float dn = dot(nrm, p.air);
                if (dn < 0) p.air = p.air + nrm * (dn * -3.f);
            }
        }
        if (m == 2 || m == 3 || m == 7) dv.y += t.gravity;
        p.vel = p.vel + dv * dt;
        p.pos = p.pos + p.vel * dt;
    }
    parts_.erase(std::remove_if(parts_.begin(), parts_.end(), [](const Particle& p) { return p.age >= 1.f; }), parts_.end());
}

// ---------------------------------------------------------------- weather (FUN_100f6c90 .. FUN_100f8630)

void Particles::setWeatherType(int type) {
    w_ = Weather{};
    w_.type = type == 1 ? 1 : type == 2 ? 2 : 0;
    if (!w_.type) return;
    w_.snow = w_.type == 2;
    // constructor FUN_100f71f0: rain falls at 30, drops live 1..3 s; snow 3, 3..5 s, wider and shorter
    w_.fall = w_.snow ? 3.f : 30.f;
    w_.lifeMin = w_.snow ? 3.f : 1.f, w_.lifeMax = w_.snow ? 5.f : 3.f;
    w_.width = w_.snow ? 1.25f : 1.f;
    w_.length = w_.snow ? 1.5f : 5.f;
    w_.lightningRate = w_.snow ? 0 : 100;
    w_.drops.assign(20 * 20, Drop{});  // allocated for 20 x 20, 7 x 7 in use
    w_.n = 7;
    setWeatherLevel(0);  // FUN_100f6c90 ends with FUN_100f6ec0(0)
}

void Particles::setWeatherLevel(int level) {
    if (!w_.type) return;
    // FUN_100f7110: the particle setting caps the level (low: no drops at all, medium: up to the second step)
    if (detail_ == 0) {
        if (level >= 0 && level < 20) level = level / 5 * 5;
    } else if (detail_ == 2) {
        if (level % 5 >= 3 && level >= 0 && level < 20) level = level / 5 * 5 + 2;
    }
    // FUN_100f6ec0: {on, rain speed, snow speed, lightning per second, grid side, drops drawn, rain sound}
    bool on = true;
    int rain = 30, snow = 3, side = 7, draw = 500, sound = -1;
    unsigned light = 0;
    if (level >= 0 && level < 20) {
        int tier = level % 5;
        light = level >= 15 ? 0 : level >= 10 ? 600 : level >= 5 ? 100 : 0;
        static const int kRain[5] = {0, 60, 80, 90, 100}, kSnow[5] = {0, 4, 7, 9, 12}, kSide[5] = {0, 5, 7, 11, 15},
                         kDraw[5] = {0, 500, 750, 1000, 1500}, kSound[5] = {-1, 0, 0, 1, 1};
        on = tier != 0;
        rain = kRain[tier], snow = kSnow[tier], side = kSide[tier], draw = kDraw[tier], sound = kSound[tier];
    }
    // FUN_100f7560
    w_.enabled = on;
    w_.sound = sound;
    w_.fall = float(w_.snow ? snow : rain);
    w_.lightning = 0, w_.lightningRate = light;
    if (size_t(side) * size_t(side) > w_.drops.size()) w_.drops.assign(size_t(side) * size_t(side), Drop{});
    w_.n = side;
    w_.maxDraw = draw;
}

void Particles::stepWeather(float dt) {
    if (!w_.type) return;
    // FUN_100f7750: every drop falls along DAT_1026b1c0 at the fall speed; spent drops start again over the grid
    Vec3 v = fallDir_ * (w_.fall * dt);
    int n = w_.n, count = n * n;
    for (int i = 0; i < count && size_t(i) < w_.drops.size(); i++) {
        Drop& d = w_.drops[size_t(i)];
        if (d.left < 0.0078125f) {
            d.life = (w_.lifeMax - w_.lifeMin) * rnd() + w_.lifeMin;
            d.left = d.life;
            // FUN_100f7a20: cell (i % n, i / n) of a 32-unit square; the jitter multiplies the cell offset (as the
            // original computes it), so the drops gather toward the tile's corner axes
            float h = 16.f / float(n), lo = d.life * 0 + w_.lifeMin * w_.fall;
            int q = i / n, r = i % n;
            float jA = rnd() * (2 * h) - h;
            float y = (50.f - lo) * rnd() + lo;
            float jB = rnd() * (2 * h) - h;
            d.pos = Vec3((32.f * float(r) / float(n)) * jB, y, jA * (-32.f * float(q) / float(n)));
            if (w_.snow) d.wob = Vec3(rnd() * 2 - 1, 0, rnd() * 2 - 1);
        }
        if (w_.snow) d.pos = d.pos + d.wob * (std::cos(d.left / d.life * 31.4159265f) * 0.2f) + v;
        else d.pos = d.pos + v;
        d.left -= dt;
    }
    // lightning (FUN_100f7b50, rain only)
    if (w_.snow) return;
    if (w_.lightning >= 0x1fff && unsigned(rng_() & 0x7fff) + 1 < w_.lightning) {
        // FUN_100f7c40: 50..180 m from the camera in a random direction; envthnd1.wav there
        float dist = 50.f + rnd() * 130.f, ang = (rnd() + rnd()) * 3.14159265f;
        Vec3 at = eye_ + Vec3(std::cos(ang) * dist, 0, std::sin(ang) * dist);
        if (groundHeight) at.y = groundHeight(at.x, at.z);
        if (onLightning) onLightning(at);
        w_.lightning -= unsigned(int(rnd() * (60000.f - 1000.f) + 1000.f));
        if (w_.lightning > 0x7fff) w_.lightning = 0;
    } else {
        if (w_.lightningRate) w_.lightning += unsigned(std::max(1, int(float(w_.lightningRate) * dt)));
        if (w_.lightning > 0x7fff) w_.lightning = 0;
    }
}

// ---------------------------------------------------------------- drawing

void Particles::submit(const Mat4& view, const Mat4& proj) {
    // camera axes from the view matrix (row-vector, left-handed: columns hold right / up / forward)
    Vec3 right(view.m[0][0], view.m[1][0], view.m[2][0]), up(view.m[0][1], view.m[1][1], view.m[2][1]),
        fwd(view.m[0][2], view.m[1][2], view.m[2][2]);
    // the original's projection factor (renderer +0x44) in pixels at its 640 x 480 screen
    float focal = proj.m[1][1] * 240.f;
    struct Key { int tex; int blend; bool operator<(const Key& o) const { return tex != o.tex ? tex < o.tex : blend < o.blend; } };
    std::map<Key, std::vector<Vertex>> byTex;  // blend: 0 alpha, 1 additive
    auto vert = [](const Vec3& q, float u, float w, const uint8_t c[4]) {
        Vertex o{};
        o.normal[1] = 1;
        o.pos[0] = q.x, o.pos[1] = q.y, o.pos[2] = q.z, o.uv[0] = u, o.uv[1] = w;
        memcpy(o.color, c, 4);
        return o;
    };
    // FUN_10128fd0: a camera-aligned square, half-size h; the frame's cell runs v at the bottom edge to v + cellH at
    // the top
    auto quad = [&](std::vector<Vertex>& dst, const Vec3& c, float h, const uint8_t col[4], float u, float v, float cw, float ch,
                    int rot = 0) {
        Vec3 ax = right * h, ay = up * h;
        float uvs[4][2] = {{u, v + ch}, {u, v}, {u + cw, v}, {u + cw, v + ch}};  // top-left, bottom-left, bottom-right, top-right
        Vec3 ps[4] = {c - ax + ay, c - ax - ay, c + ax - ay, c + ax + ay};
        Vertex vs[4];
        for (int k = 0; k < 4; k++) vs[k] = vert(ps[k], uvs[(k + rot) & 3][0], uvs[(k + rot) & 3][1], col);
        dst.insert(dst.end(), {vs[0], vs[1], vs[2], vs[0], vs[2], vs[3]});
    };
    auto flat = [&](std::vector<Vertex>& dst, const Vec3& c, float h, const uint8_t col[4], float u, float v, float cw, float ch) {
        Vec3 ps[4] = {c + Vec3(-h, 0, h), c + Vec3(-h, 0, -h), c + Vec3(h, 0, -h), c + Vec3(h, 0, h)};  // FUN_10129530
        float uvs[4][2] = {{u, v + ch}, {u, v}, {u + cw, v}, {u + cw, v + ch}};
        Vertex vs[4];
        for (int k = 0; k < 4; k++) vs[k] = vert(ps[k], uvs[k][0], uvs[k][1], col);
        dst.insert(dst.end(), {vs[0], vs[1], vs[2], vs[0], vs[2], vs[3]});
    };
    std::unordered_map<int, const Particle*> byId;
    for (auto& p : parts_) byId[p.id] = &p;
    for (auto& p : parts_) {
        const ParticleType& t = *p.type;
        int i = std::min(255, std::max(0, int(p.age * 256.f)));
        const float* c = t.rgba[i];
        uint8_t col[4] = {uint8_t(c[0] * 255), uint8_t(c[1] * 255), uint8_t(c[2] * 255), uint8_t(c[3] * 255)};
        if ((t.mode == 1 || t.mode == 9) && p.tint != 0xffffffffu)  // the stream colour replaces the RGB
            col[0] = uint8_t(p.tint >> 16), col[1] = uint8_t(p.tint >> 8), col[2] = uint8_t(p.tint);
        float h = t.size[i] * p.scale;
        int fr = 0;
        if (t.frames > 1 && t.mode != 0) fr = int(p.age * t.anim * float(t.frames)) % t.frames;
        float u = t.uv[fr][0], v = t.uv[fr][1];
        auto& dst = byTex[{t.tex, (t.flags & 0x100) ? 1 : 0}];
        switch (t.mode) {
        case 0: case 1: case 10: case 11:
            quad(dst, p.pos, h, col, u, v, t.cellW, t.cellH);
            break;
        case 3: {  // sized on screen: half-size x depth / projection factor
            float depth = dot(p.pos - eye_, fwd);
            quad(dst, p.pos, h * (depth * h / std::max(1.f, focal)), col, u, v, t.cellW, t.cellH);
            break;
        }
        case 2: case 9: {  // FUN_10129230: sprites fill the gap back to the particle made before this one
            auto it = byId.find(p.link);
            if (it == byId.end()) break;
            const Particle& o = *it->second;
            int oi = std::min(255, std::max(0, int(o.age * 256.f)));
            float oh = o.type->size[oi] * o.scale;
            Vec3 d = o.pos - p.pos;
            int cnt = 1;
            if (h > 0.01f) cnt = 1 + int(length(d) / (h * 2.f));
            cnt = std::max(1, cnt);
            Vec3 at = p.pos, sv = d * (1.f / float(cnt));
            float hs = h, dh = (oh - h) / float(cnt);
            for (int k = 0; k < cnt; k++, at = at + sv, hs += dh) quad(dst, at, hs, col, u, v, t.cellW, t.cellH, k);
            break;
        }
        case 4:
            flat(dst, p.pos, h, col, u, v, t.cellW, t.cellH);
            break;
        case 6:  // under water a sprite, else on the surface
            if (p.pos.y + h * 2.f < water_) quad(dst, p.pos, h, col, u, v, t.cellW, t.cellH);
            else flat(dst, Vec3(p.pos.x, water_ + 0.1f, p.pos.z), h, col, u, v, t.cellW, t.cellH);
            break;
        case 8:  // above water a sprite, else on the surface
            if (water_ < p.pos.y) quad(dst, p.pos, h, col, u, v, t.cellW, t.cellH);
            else flat(dst, Vec3(p.pos.x, water_ + 0.1f, p.pos.z), h, col, u, v, t.cellW, t.cellH);
            break;
        case 5:
            flat(dst, Vec3(p.pos.x, water_ + 0.1f, p.pos.z), h, col, u, v, t.cellW, t.cellH);
            break;
        default:
            break;
        }
    }
    // weather (FUN_100f8160): the drop field drawn over each visible terrain tile, nearest first, until the frame's
    // allowance is used up. Rain: a thin untextured sliver from the drop 5 m along the fall, 1 pixel each side at its
    // far end, white at alpha 20; over water a drop below the surface draws a growing ring there instead
    // (effect2c.tga 0.5..0.75 x 0..0.25). Snow flakes are three points on one line (FUN_100f8390's snow branch) and
    // draw nothing.
    if (w_.type == 1 && w_.enabled && groundHeight) {
        auto& streaks = byTex[{-1, 0}];
        int splashTex = texture("effect2c.tga");
        auto& rings = byTex[{splashTex, 0}];
        const uint8_t rainCol[4] = {255, 255, 255, 20}, ringCol[4] = {0xdc, 0xe6, 0xff, 0xd2};
        float ex = proj.m[0][0], ey = proj.m[1][1];
        int tx0 = int(std::floor(eye_.x / 32.f)), tz0 = int(std::floor(eye_.z / 32.f));
        std::vector<std::pair<int, std::pair<int, int>>> tiles;
        for (int dz = -kTileReach; dz <= kTileReach; dz++)
            for (int dx = -kTileReach; dx <= kTileReach; dx++) tiles.push_back({dx * dx + dz * dz, {tx0 + dx, tz0 + dz}});
        std::sort(tiles.begin(), tiles.end());
        int drawn = 0, n = w_.n;
        Vec3 dirW = fallDir_ * (1.f / length(fallDir_));
        for (auto& tt : tiles) {
            if (drawn > w_.maxDraw) break;
            int tx = tt.second.first, tz = tt.second.second;
            if (tx < 0 || tz < 0 || tx >= 128 || tz >= 128) continue;
            float x0 = float(tx) * 32.f, z0 = float(tz) * 32.f, maxH = -1e9f, minH = 1e9f;
            for (int a = 0; a <= 4; a++)
                for (int b = 0; b <= 4; b++) {
                    float hh = std::floor(groundHeight(x0 + float(a) * 8.f, z0 + float(b) * 8.f));
                    maxH = std::max(maxH, hh), minH = std::min(minH, hh);
                }
            Vec3 base(x0, maxH, z0);
            bool water = hasWater_ && minH < water_;
            for (int k = 0; k < n * n && size_t(k) < w_.drops.size(); k++) {
                const Drop& d = w_.drops[size_t(k)];
                Vec3 P = base + d.pos;
                Vec3 rel = P - eye_;
                float depth = dot(rel, fwd);
                if (depth < 0.5f - w_.length) continue;
                float sx = dot(rel, right), sy = dot(rel, up), r = w_.length;  // sphere of the streak length (FUN_100058b0)
                if (std::fabs(sx) * ex > depth + r * 1.5f || std::fabs(sy) * ey > depth + r * 1.5f) continue;
                if (water && P.y < water_ + 0.08f) {
                    float tl = (water_ - P.y) / dirW.y;
                    Vec3 S = P + dirW * tl;
                    S.y = water_;
                    float g = (1.f - d.left / std::max(1e-3f, d.life)) * w_.splash;
                    float rr = g * g;
                    Vec3 q[4] = {S + Vec3(rr, 0, rr), S + Vec3(rr, 0, -rr), S + Vec3(-rr, 0, -rr), S + Vec3(-rr, 0, rr)};
                    float uv[4][2] = {{0.5f, 0.f}, {0.75f, 0.f}, {0.75f, 0.25f}, {0.5f, 0.25f}};
                    Vertex vs[4];
                    for (int m = 0; m < 4; m++) vs[m] = vert(q[m], uv[m][0], uv[m][1], ringCol);
                    rings.insert(rings.end(), {vs[0], vs[1], vs[2], vs[0], vs[2], vs[3]});
                } else {
                    // the fall direction on screen and its perpendicular
                    Vec3 dc(dot(dirW, right), dot(dirW, up), 0);
                    float l2 = std::sqrt(dc.x * dc.x + dc.y * dc.y);
                    Vec3 perp = l2 > 0.001f ? right * (-dc.y / l2) + up * (dc.x / l2) : right;
                    Vec3 wv = perp * (std::max(0.f, depth) * w_.width / std::max(1.f, focal));
                    Vec3 E = P + dirW * w_.length;
                    streaks.push_back(vert(P - wv, 0, 0, rainCol));
                    streaks.push_back(vert(E - wv, 0, 0, rainCol));
                    streaks.push_back(vert(E + wv, 0, 0, rainCol));
                }
                drawn++;
            }
        }
    }
    std::vector<Vertex> all;
    std::vector<DrawBatch> batches;
    for (auto& [key, vs] : byTex) {
        if (vs.empty()) continue;
        DrawBatch b;
        b.texture = key.tex, b.first = uint32_t(all.size()), b.count = uint32_t(vs.size()), b.blend = true, b.additive = key.blend == 1;
        batches.push_back(b);
        all.insert(all.end(), vs.begin(), vs.end());
    }
    r_->setDynamic(all, batches);
}

}  // namespace hg2
