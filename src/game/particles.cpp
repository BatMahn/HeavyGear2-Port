#include "particles.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include "../assets/assets.h"

namespace hg2 {

namespace {
float f32(const Bytes& b, size_t off) {
    float v = 0;
    if (off + 4 <= b.size()) memcpy(&v, b.data() + off, 4);
    return v;
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
constexpr float kSizeScale = 4.f;  // size curve units -> metres (matches the original's look at gear scale)
constexpr size_t kMaxParticles = 4000;
}  // namespace

void Particles::init(const FileSystem& fs, Renderer& r, const std::string& detail) {
    fs_ = &fs, r_ = &r;
    dir_ = "particle system/" + detail + "/";
}

bool Particles::read(const std::string& file, Bytes& out) const {
    return fs_ && (fs_->read(dir_ + file, out) || fs_->read("particle system/hi/" + file, out));
}

const ParticleType* Particles::type(const std::string& name) {
    std::string k = upper(name);
    auto it = types_.find(k);
    if (it != types_.end()) return it->second.get();
    std::unique_ptr<ParticleType> t;
    Bytes b;
    if (read(name + ".ptl", b) && b.size() >= 0x1aa0) {
        t = std::make_unique<ParticleType>();
        t->name = cstr(b, 0, 256);
        t->life = std::max(0.05f, f32(b, 0x100));
        t->flags = u32(b, 0x108);
        t->gravity = f32(b, 0x110);
        t->texture = cstr(b, 0x118, 256);
        t->frames = std::max(1, std::min(16, int(u32(b, 0x2a4))));
        t->u0 = std::max(0.f, std::min(0.75f, f32(b, 0x21c)));
        t->v0 = std::max(0.f, std::min(0.75f, f32(b, 0x220)));
        if (t->frames > 1) t->u0 = t->v0 = 0, t->cellW = 0.25f, t->cellH = 0.1875f;
        else t->cellW = t->cellH = 0.25f;
        for (int i = 0; i < 256; i++) {
            for (int c = 0; c < 4; c++) t->rgba[i][c] = std::max(0.f, std::min(1.f, f32(b, 0x2ac + size_t(c) * 0x400 + size_t(i) * 4)));
            t->size[i] = std::max(0.f, f32(b, 0x16ac + size_t(i) * 4));
        }
        if (!t->texture.empty()) {
            std::string tk = upper(t->texture);
            auto ti = textures_.find(tk);
            if (ti != textures_.end()) {
                t->tex = ti->second;
            } else {
                Bytes tb;
                Image img;
                if (fs_->read(t->texture, tb) && decodeImage(tb, t->texture, img)) {
                    // effect textures are black-backed: derive alpha from brightness where there is none
                    bool opaque = true;
                    for (size_t p = 3; p < img.rgba.size(); p += 4) opaque &= img.rgba[p] == 255;
                    if (opaque)
                        for (size_t p = 0; p < img.rgba.size(); p += 4)
                            img.rgba[p + 3] = std::max(img.rgba[p], std::max(img.rgba[p + 1], img.rgba[p + 2]));
                    t->tex = r_->createTexture(img);
                }
                textures_[tk] = t->tex;
            }
        }
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
        e->duration = f32(b, 0x10c);
        e->count = int(u32(b, 0x114));
        e->speed = f32(b, 0x118);
        e->speedVar = f32(b, 0x11c);
        e->cone = f32(b, 0x120);
        if (b.size() >= 0x928) {
            bool ok = true;
            for (int i = 0; i < 256; i++) {
                e->rate[i] = f32(b, 0x128 + size_t(i) * 4), e->speedCurve[i] = f32(b, 0x528 + size_t(i) * 4);
                ok = ok && std::isfinite(e->rate[i]) && e->rate[i] >= 0 && e->rate[i] < 1e6f && std::isfinite(e->speedCurve[i]) &&
                     std::fabs(e->speedCurve[i]) < 1e4f;
            }
            e->hasCurves = ok;
        }
        if (!(e->duration >= 0 && e->duration < 1000)) e->duration = 1;
        if (e->count < 0 || e->count > 10000) e->count = 8;
    }
    auto* raw = e.get();
    emitters_[k] = std::move(e);
    return raw;
}

const Effect* Particles::effect(const std::string& name) {
    std::string k = upper(name);
    auto it = effects_.find(k);
    if (it != effects_.end()) return it->second.get();
    auto fx = std::make_unique<Effect>();
    Bytes b;
    if (read(name + ".ped", b) && b.size() >= 0x104) {
        uint32_t n = std::min<uint32_t>(u32(b, 0x100), 32);
        for (uint32_t i = 0; i < n; i++) {
            size_t o = 0x104 + size_t(i) * 0x218;
            if (o + 0x218 > b.size()) break;
            Effect::Part p{emitter(cstr(b, o, 256)), type(cstr(b, o + 0x100, 256)), f32(b, o + 0x204), f32(b, o + 0x208),
                           f32(b, o + 0x20c), f32(b, o + 0x214)};
            if (p.type) fx->parts.push_back(p);
        }
    } else if (const ParticleType* t = type(name)) {  // emitter + particle of the same name, or a lone particle
        fx->parts.push_back({emitter(name), t, 1.f, 0.f, 0.f, 0.f});
    }
    if (fx->parts.empty()) fx.reset();
    auto* raw = fx.get();
    effects_[k] = std::move(fx);
    return raw;
}

void Particles::emit(const Effect::Part& p, const Vec3& pos, const Vec3& up, float scale, int n, float speedMul, int stream) {
    std::uniform_real_distribution<float> U(0.f, 1.f);
    // basis around `up`
    Vec3 a = std::fabs(up.y) < 0.9f ? Vec3(0, 1, 0) : Vec3(1, 0, 0);
    Vec3 t1 = normalize(cross(up, a)), t2 = cross(up, t1);
    float cone = (p.cone > 0 ? p.cone : p.emitter ? p.emitter->cone : 90.f) * 3.14159265f / 180.f;
    for (int i = 0; i < n && parts_.size() < kMaxParticles; i++) {
        float th = std::acos(1.f - U(rng_) * (1.f - std::cos(std::min(3.14159f, cone)))), ph = U(rng_) * 6.2831853f;
        Vec3 dir = up * std::cos(th) + (t1 * std::cos(ph) + t2 * std::sin(ph)) * std::sin(th);
        float sp = p.emitter ? p.emitter->speed + p.emitter->speedVar * (U(rng_) * 2 - 1) : 0.f;
        if (p.velocity > 0) sp = std::max(sp, p.velocity * (0.5f + U(rng_) * 0.5f));
        sp *= speedMul;
        Particle q;
        q.pos = pos + dir * (0.3f * scale);
        q.vel = dir * (sp * scale);
        q.age = 0;
        q.life = p.type->life * (0.8f + 0.4f * U(rng_));
        q.scale = scale * (p.scale > 0 ? p.scale : 1.f);
        q.spin = U(rng_) * 6.2831853f;
        q.type = p.type;
        q.stream = stream;
        parts_.push_back(q);
    }
}

bool Particles::spawn(const std::string& name, const Vec3& pos, float scale, const Vec3& up, float density) {
    const Effect* fx = effect(name);
    if (!fx) return false;
    for (auto& p : fx->parts) {
        int total = p.emitter ? std::max(1, std::min(int(float(p.emitter->count) * density + 0.5f), 80)) : 1;
        if (!p.emitter || !p.emitter->hasCurves) {  // no emitter curves: a burst, or spread over the emission time
            float dur = p.emitter ? p.emitter->duration : 0.f;
            if (dur <= 1.f && p.delay <= 0) {
                emit(p, pos, up, scale, std::min(total, 40));
                continue;
            }
        }
        streams_.push_back({p, pos, up, scale, -p.delay, 0.f, total, nextStream_++, 0});
    }
    return true;
}

int Particles::dotTexture() {
    if (dotTex_ < 0) {
        Image dot;  // soft round spot (snow, light glows)
        dot.w = dot.h = 32;
        dot.rgba.assign(32 * 32 * 4, 255);
        for (int y = 0; y < 32; y++)
            for (int x = 0; x < 32; x++) {
                float dx = (float(x) - 15.5f) / 15.5f, dy = (float(y) - 15.5f) / 15.5f, f = std::max(0.f, 1.f - (dx * dx + dy * dy));
                dot.rgba[size_t(y * 32 + x) * 4 + 3] = uint8_t(255.f * f * f);
            }
        dotTex_ = r_->createTexture(dot, false);
    }
    return dotTex_;
}

void Particles::update(float dt) {
    // weather: drops / flakes in a box around the viewer
    if (weatherType_ > 0 && weatherLevel_ > 0) {
        if (!rain_) {
            rain_ = std::make_unique<ParticleType>(), snow_ = std::make_unique<ParticleType>();
            rain_->life = 1.2f, snow_->life = 6.f;
            snow_->tex = dotTexture();
            for (int i = 0; i < 256; i++) {
                rain_->rgba[i][0] = 0.75f, rain_->rgba[i][1] = 0.8f, rain_->rgba[i][2] = 0.9f, rain_->rgba[i][3] = 0.6f;
                snow_->rgba[i][0] = snow_->rgba[i][1] = snow_->rgba[i][2] = 1.f, snow_->rgba[i][3] = 0.85f;
                rain_->size[i] = 0.1f, snow_->size[i] = 0.09f;
            }
        }
        const ParticleType* t = weatherType_ == 2 ? snow_.get() : rain_.get();
        std::uniform_real_distribution<float> U(-1.f, 1.f);
        weatherAcc_ += dt * float(weatherLevel_) * (weatherType_ == 2 ? 120.f : 400.f);
        for (; weatherAcc_ >= 1.f && parts_.size() < kMaxParticles; weatherAcc_ -= 1.f) {
            Particle q;
            q.pos = eye_ + Vec3(U(rng_) * 30.f, (weatherType_ == 2 ? 8.f : 15.f) + U(rng_) * 8.f, U(rng_) * 30.f);
            q.vel = weatherType_ == 2 ? Vec3(U(rng_) * 1.5f, -2.5f, U(rng_) * 1.5f) : Vec3(0.5f, -22.f, 0.3f);
            q.age = 0, q.life = t->life, q.scale = 1.f, q.spin = 0, q.type = t;
            parts_.push_back(q);
        }
    }
    if (!streams_.empty()) {  // particles still alive per stream
        std::map<int, int> alive;
        for (auto& p : parts_)
            if (p.stream) alive[p.stream]++;
        for (auto& s : streams_) s.alive = alive[s.id];
    }
    for (auto& s : streams_) {
        s.t += dt;
        if (s.t < 0) continue;
        const Emitter* e = s.part.emitter;
        float dur = std::max(0.05f, e ? e->duration : 1.f);
        if (s.t > dur) continue;
        if (e && e->hasCurves) {
            int k = std::min(255, int(s.t / dur * 255.f));
            s.acc += e->rate[k] * dt;
            int n = std::min(int(s.acc), s.total - s.alive);
            if (n > 0) emit(s.part, s.pos, s.up, s.scale, n, std::max(0.f, e->speedCurve[k]), s.id), s.alive += n;
            s.acc -= std::floor(s.acc);  // what can't be emitted (at the cap) is dropped, not saved up
        } else {  // evenly over the emission time
            float want = float(s.total) * std::min(1.f, s.t / dur);
            int n = int(want - s.acc);
            if (n > 0) emit(s.part, s.pos, s.up, s.scale, n, 1.f, s.id), s.acc += float(n);
        }
    }
    streams_.erase(std::remove_if(streams_.begin(), streams_.end(),
                                  [](const Stream& s) { return s.t > std::max(0.05f, s.part.emitter ? s.part.emitter->duration : 1.f); }),
                   streams_.end());
    for (auto& p : parts_) {
        p.age += dt;
        p.vel.y -= p.type->gravity * dt;
        p.vel = p.vel * std::max(0.f, 1.f - 0.6f * dt);  // air drag
        p.pos += p.vel * dt;
    }
    parts_.erase(std::remove_if(parts_.begin(), parts_.end(), [](const Particle& p) { return p.age >= p.life; }), parts_.end());
}

void Particles::submit(const Mat4& view) {
    // camera right / up from the view matrix (row-vector: columns hold the camera axes)
    Vec3 right(view.m[0][0], view.m[1][0], view.m[2][0]), up(view.m[0][1], view.m[1][1], view.m[2][1]);
    std::map<std::pair<int, bool>, std::vector<Vertex>> byTex;
    for (auto& p : parts_) {
        int i = std::min(255, int(p.age / p.life * 255.f));
        const float* c = p.type->rgba[i];
        float sz = std::min(6.f, p.type->size[i] * kSizeScale * p.scale * 0.5f);
        if (sz <= 0.001f || c[3] <= 0.004f) continue;
        // sprites right in front of the viewer fade out instead of filling the screen
        float near = length(p.pos - eye_) / std::max(1.f, sz * 3.f);
        float fade = std::min(1.f, near * near);
        if (fade < 0.05f) continue;
        float cs = std::cos(p.spin) * sz, sn = std::sin(p.spin) * sz;
        Vec3 ax = right * cs + up * sn, ay = up * cs - right * sn;
        if (p.type == rain_.get()) {  // rain falls as streaks along its motion
            Vec3 fwd = cross(right, up), d = normalize(p.vel), side = normalize(cross(d, fwd));
            ax = side * 0.025f, ay = d * 0.7f;
        }
        Vertex v{};
        v.normal[1] = 1;
        v.color[0] = uint8_t(c[0] * 255), v.color[1] = uint8_t(c[1] * 255), v.color[2] = uint8_t(c[2] * 255),
        v.color[3] = uint8_t(c[3] * 255 * fade);
        auto mk = [&](const Vec3& q, float u, float w) {
            Vertex o = v;
            o.pos[0] = q.x, o.pos[1] = q.y, o.pos[2] = q.z, o.uv[0] = u, o.uv[1] = w;
            return o;
        };
        float cu = p.type->u0, cv = p.type->v0;
        if (p.type->frames > 1) {
            int fr = std::min(p.type->frames - 1, int(p.age / p.life * float(p.type->frames)));
            cu = float(fr % 4) * p.type->cellW, cv = float(fr / 4) * p.type->cellH;
        }
        float cu1 = cu + p.type->cellW, cv1 = cv + p.type->cellH;
        Vertex a = mk(p.pos - ax - ay, cu, cv1), b = mk(p.pos + ax - ay, cu1, cv1), cc = mk(p.pos + ax + ay, cu1, cv),
               d = mk(p.pos - ax + ay, cu, cv);
        auto& dst = byTex[{p.type->tex, (p.type->flags & 0x100) != 0}];
        dst.insert(dst.end(), {a, b, cc, a, cc, d});
    }
    for (auto& f : flares_) {
        Vertex v{};
        v.normal[1] = 1;
        v.color[0] = f.rgb[0], v.color[1] = f.rgb[1], v.color[2] = f.rgb[2], v.color[3] = 220;
        auto mk = [&](const Vec3& q, float u, float w) {
            Vertex o = v;
            o.pos[0] = q.x, o.pos[1] = q.y, o.pos[2] = q.z, o.uv[0] = u, o.uv[1] = w;
            return o;
        };
        Vec3 ax = right * f.size, ay = up * f.size;
        Vertex a = mk(f.pos - ax - ay, 0, 1), b = mk(f.pos + ax - ay, 1, 1), cc = mk(f.pos + ax + ay, 1, 0), d = mk(f.pos - ax + ay, 0, 0);
        auto& dst = byTex[{dotTexture(), true}];
        dst.insert(dst.end(), {a, b, cc, a, cc, d});
    }
    flares_.clear();
    std::vector<Vertex> all;
    std::vector<DrawBatch> batches;
    for (auto& [key, vs] : byTex) {
        DrawBatch b;
        b.texture = key.first, b.first = uint32_t(all.size()), b.count = uint32_t(vs.size()), b.blend = true, b.additive = key.second;
        batches.push_back(b);
        all.insert(all.end(), vs.begin(), vs.end());
    }
    r_->setDynamic(all, batches);
}

}  // namespace hg2
