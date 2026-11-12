// Particle effects from the game's "Particle System" folder: .ped effects (emitter + particle pairs),
// .emt emitters and .ptl particle types (fixed-size records read straight into memory by
// FUN_10126a30 / FUN_10127110 / FUN_101272a0). Drawn as camera-facing billboards.
#pragma once
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "../core/fs.h"
#include "../core/math.h"
#include "../render/renderer.h"

namespace hg2 {

struct ParticleType {          // .ptl, 0x1ab4 bytes
    std::string name, texture;
    int tex = -1;
    float life = 1;            // +0x100 seconds
    float gravity = 0;         // +0x110
    uint32_t flags = 0;        // +0x108 (0x100: additive)
    float rgba[256][4];        // colour over life: R +0x2ac, G +0x6ac, B +0xaac, A +0xeac (256 floats each)
    float size[256];           // size over life, +0x16ac
    // texture atlas cell: 4x4 grid of 32-pixel cells; +0x21c u0, +0x220 v0 of the sprite. Animated types
    // (+0x2a4 frames > 1, smokmuzc.tga) step through 0.25 x 0.1875 cells in reading order over their life.
    float u0 = 0, v0 = 0, cellW = 1, cellH = 1;
    int frames = 1;
};
struct Emitter {               // .emt, 0x928 bytes
    std::string name;
    uint32_t flags = 0;
    float duration = 1;        // +0x10c emission time
    int count = 8;             // +0x114
    float speed = 1, speedVar = 0, cone = 90;  // +0x118, +0x11c, +0x120 (degrees)
    // curves over the emission time (256 steps): +0x128 emission rate (particles per second), +0x528 speed factor
    float rate[256] = {}, speedCurve[256] = {};
    bool hasCurves = false;
};
struct Effect {                // .ped: n x {emitter name[256], particle name[256], u32, f32 scale, f32 delay, f32 velocity, f32, f32 cone}
    struct Part { const Emitter* emitter; const ParticleType* type; float scale, delay, velocity, cone; };
    std::vector<Part> parts;
};

class Particles {
public:
    void init(const FileSystem& fs, Renderer& r, const std::string& detail = "hi");
    // Start an effect by name (.ped, or an .emt/.ptl pair, or a lone .ptl). `up` orients the emission cone.
    // `density` scales the particle counts (weapon hits use a fraction so sustained fire stays readable).
    bool spawn(const std::string& name, const Vec3& pos, float scale = 1.f, const Vec3& up = Vec3(0, 1, 0), float density = 1.f);
    void update(float dt);
    // Builds the billboards for this camera and hands them to the renderer (before beginFrame).
    void submit(const Mat4& view);
    size_t live() const { return parts_.size(); }
    // Precipitation around the viewer (SetWeatherType 1 rain / 2 snow, SetWeather preset: level = preset % 5).
    void setWeather(int type, int level) { weatherType_ = type, weatherLevel_ = level; }
    void setViewer(const Vec3& eye) { eye_ = eye; }
    // A glow drawn this frame only (mission lights).
    void flare(const Vec3& pos, const uint8_t rgb[3], float size) { flares_.push_back({pos, {rgb[0], rgb[1], rgb[2]}, size}); }

private:
    const Effect* effect(const std::string& name);
    const ParticleType* type(const std::string& name);
    const Emitter* emitter(const std::string& name);
    bool read(const std::string& file, Bytes& out) const;

    struct Particle { Vec3 pos, vel; float age, life, scale, spin; const ParticleType* type; int stream = 0; };
    // One emitter running: it emits at its rate curve for its emission time, keeping at most `total` of its
    // particles alive.
    struct Stream { Effect::Part part; Vec3 pos, up; float scale, t, acc; int total, id, alive; };
    void emit(const Effect::Part& p, const Vec3& pos, const Vec3& up, float scale, int n, float speedMul = 1.f, int stream = 0);
    int nextStream_ = 1;

    const FileSystem* fs_ = nullptr;
    Renderer* r_ = nullptr;
    std::string dir_;
    std::map<std::string, std::unique_ptr<ParticleType>> types_;
    std::map<std::string, std::unique_ptr<Emitter>> emitters_;
    std::map<std::string, std::unique_ptr<Effect>> effects_;
    std::map<std::string, int> textures_;
    std::vector<Particle> parts_;
    std::vector<Stream> streams_;
    std::mt19937 rng_{1234};
    int weatherType_ = 0, weatherLevel_ = 0;
    Vec3 eye_;
    float weatherAcc_ = 0;
    std::unique_ptr<ParticleType> rain_, snow_;
    struct Flare { Vec3 pos; uint8_t rgb[3]; float size; };
    std::vector<Flare> flares_;
    int dotTex_ = -1;
    int dotTexture();
};

}  // namespace hg2
