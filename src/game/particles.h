// Particle effects from the game's "Particle System" folder: .ped effects (emitter + particle pairs),
// .emt emitters and .ptl particle types (fixed-size records read straight into memory by
// FUN_10126a30 / FUN_10127110 / FUN_101272a0), and the weather (rain / snow, FUN_100f71f0). All of it is the
// original's model as decoded in docs/FORMATS.md ("Particle System", "Weather").
#pragma once
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "../core/fs.h"
#include "../core/math.h"
#include "../render/renderer.h"

namespace hg2 {

struct ParticleType {          // .ptl, 0x1ab4 bytes (FUN_10126a30)
    std::string name, texture;
    int tex = -1;
    float life = 1, lifeVar = 0;   // +0x100 seconds, +0x104 random extra (0..var)
    uint32_t flags = 0;            // +0x108: bit 0 drag, bit 1 gravity, 0x100 additive
    float drag = 0;                // +0x110 rate (1/s) toward the air velocity
    float gravity = 0;             // +0x114 x 9.8 (added to the vertical speed; smoke rises, sparks fall)
    bool wind = false;             // +0x1aac bit 31: the air velocity is the wind (FUN_10127920)
    float anim = 0;                // +0x1ab0 frame cycles over the life (0 in the short 0x1ab0-byte files)
    float rgba[256][4];            // colour over life: R +0x2ac, G +0x6ac, B +0xaac, A +0xeac
    float size[256];               // +0x16ac, halved at load: the sprite's half-size at scale 1
    float uv[16][2] = {};          // +0x21c: texture cell origin per frame
    float cellW = 0.25f, cellH = 0.25f;  // +0x29c / +0x2a0 cell size (negative heights flip the cell)
    int frames = 1;                // +0x2a4
    int mode = 0;                  // +0x2a8 draw mode (FUN_10128ac0)
};
struct Emitter {               // .emt, 0x928 bytes
    std::string name;
    uint32_t flags = 0;        // +0x100: 2 inherit the emitter's velocity, 0x10 loop, 0x20 gravity, 0x80 drag, 0x100 moves
    int shape = 0;             // +0x104: 0 point, 1 cone shell, 2 ring (FUN_10123bd0)
    float drag = 0;            // +0x108 (flag 0x80)
    float duration = 1, durationVar = 0;  // +0x10c, +0x110
    int count = 8;             // +0x114 most particles alive at once
    float speed = 1, speedVar = 0, cone = 90, radius = 0;  // +0x118, +0x11c, +0x120 (degrees), +0x124
    // curves over the emission time (256 steps): +0x128 particles per whole emission time, +0x528 speed factor
    float rate[256] = {}, speedCurve[256] = {};
};
struct Effect {                // .ped: n x {emitter name[256], particle name[256], u32 attach, f32 scale, scale var,
                               // velocity, velocity var, cone}
    struct Part { const Emitter* emitter; const ParticleType* type; uint32_t attach; float scale, scaleVar, vel, velVar, cone; };
    std::vector<Part> parts;
};

class Particles {
public:
    // detail: the original's particle setting (DAT_101fcf44 +0xb0): 0 low, 1 high, 2 medium.
    void init(const FileSystem& fs, Renderer& r, int detail = 1);
    // Terrain queries for ground-hugging particles, weather tiles and the water plane.
    std::function<float(float, float)> groundHeight;
    std::function<Vec3(float, float)> groundNormal;
    void setWater(bool has, float level) { hasWater_ = has, water_ = level; }
    // Start an effect (.ped) by name (FUN_10124480 -> FUN_101260c0). `up` is the effect's frame (the emission cone
    // axis). `density` is accepted for the callers but has no effect: the original has no such thing.
    // `tint` (0x00RRGGBB, 0xffffffff none) colours particle types of draw mode 1 / 9.
    bool spawn(const std::string& name, const Vec3& pos, float scale = 1.f, const Vec3& up = Vec3(0, 1, 0), float density = 1.f,
               uint32_t tint = 0xffffffffu);
    // An effect on a moving object: its attached parts (.ped flag 1) follow the position given by move() until
    // detach(), which ends the emitters (their particles live on). Returns 0 if the effect is missing.
    int attach(const std::string& name, const Vec3& pos, float scale = 1.f, uint32_t tint = 0xffffffffu);
    void move(int handle, const Vec3& pos, const Vec3& dir);
    void detach(int handle);
    void update(float dt);
    // Builds the sprites and the weather for this camera and hands them to the renderer (before beginFrame).
    void submit(const Mat4& view, const Mat4& proj);
    size_t live() const { return parts_.size(); }
    // SetWeatherType (FUN_100f6e80: 1 rain, 2 snow, else none; resets the level to 0) and SetWeather (FUN_100f6ec0).
    void setWeatherType(int type);
    void setWeatherLevel(int level);
    // Wind (SetGlobalWindVariables / FUN_10124c00), see FORMATS.md.
    void setWind(const Vec3& w, float gust) { wind_ = w, gust_ = gust; }
    void setViewer(const Vec3& eye) { eye_ = eye; }
    // Weather sounds: the rain loop to play (-1 none, 0 Envrain2.wav, 1 Envrain1.wav) and lightning strikes.
    int weatherType() const { return w_.type; }
    int detail() const { return detail_; }
    const Vec3& viewer() const { return eye_; }
    bool underWater(const Vec3& p) const { return hasWater_ && p.y < water_; }
    int rainSound() const { return w_.enabled ? w_.sound : -1; }
    std::function<void(const Vec3&)> onLightning;  // envthnd1.wav at the strike
    void testStrike(const Vec3& at) { strikeBolt(at); }  // test: a bolt at a chosen point

private:
    const Effect* effect(const std::string& name);
    const ParticleType* type(const std::string& name);
    const Emitter* emitter(const std::string& name);
    bool read(const std::string& file, Bytes& out) const;
    int texture(const std::string& name);
    void step(float dt);
    void stepWeather(float dt);
    float rnd() { return float(rng_() & 0x7fff) / 32767.f; }  // rand() / 32767
    Vec3 randomUnit();

    struct Particle {
        Vec3 pos, vel, air;
        float age = 0, rate = 1, scale = 1;  // age 0..1 of the life, rate = 1 / life
        const ParticleType* type;
        int motion = 0;    // FUN_10127920: (flags & 3) | 4 with wind; 8 at rest
        int id = 0, link = 0, emitter = 0;  // link: the particle the same emitter made before this one (trails)
        uint32_t tint = 0xffffffffu;
    };
    struct Inst {      // a running emitter (FUN_10123480)
        Effect::Part part;
        Vec3 pos, prev, vel, ax, ay, az;  // ay: the effect's up
        float scale = 1, t = 0, prevT = 0, rate = 1;
        int id = 0, handle = 0, alive = 0, last = 0;
        bool attached = false;
        uint32_t tint = 0xffffffffu;
    };
    void emitOne(Inst& e, const Vec3& at, float speedFactor);
    int nextId_ = 1, nextHandle_ = 1;

    const FileSystem* fs_ = nullptr;
    Renderer* r_ = nullptr;
    int detail_ = 1;
    std::string dir_;
    std::map<std::string, std::unique_ptr<ParticleType>> types_;
    std::map<std::string, std::unique_ptr<Emitter>> emitters_;
    std::map<std::string, std::unique_ptr<Effect>> effects_;
    std::map<std::string, int> textures_;
    std::vector<Particle> parts_;
    std::vector<Inst> insts_;
    std::mt19937 rng_{1234};
    Vec3 eye_;
    Vec3 wind_;
    float gust_ = 0, gustTime_ = 0, acc_ = 0;
    uint8_t phase_ = 0, gustCount_ = 0;
    bool gustOn_ = true;
    bool hasWater_ = false;
    float water_ = 0;

    // Weather object (vtable 0x1014b4d0): an N x N field of drops shared by every visible terrain tile.
    struct Drop { float life = 0, left = 0; Vec3 pos, wob; };
    struct Weather {
        int type = 0;              // 1 rain, 2 snow
        bool enabled = false;
        bool snow = false;
        float fall = 30;           // _DAT_10163638: fall speed
        float lifeMin = 1, lifeMax = 3;  // +0xdb0 / +0xdb4
        float width = 1.f;         // +0xdd0 streak / flake width (pixels at 640 x 480)
        float splash = 0.9f;       // +0xdd8 splash ring growth
        float length = 5.f;        // DAT_10163640: streak length (1.5 for snow)
        int n = 7;                 // +0xdc4 grid side
        int maxDraw = 500;         // +0xde4 drops drawn per frame
        int sound = -1;
        unsigned lightning = 0, lightningRate = 0;  // +0xe0c counter, +0xe10 per second
        std::vector<Drop> drops;
        float acc = 0;
    } w_;
    Vec3 fallDir_ = normalize(Vec3(1, -3, 0)) * 0.9999999f;  // DAT_1026b1c0
    Mat4 view_;  // the last frame's view (the lightning placement test reads it)

    // Lightning bolt (FUN_100f0400, vtable 0x1014a6a0, 0x5e0 bytes): an 80 m channel straight up from the strike
    // point, drawn as up to four jagged ribbons (+0x2c0, 200 bytes each) and a flash at the top. Each ribbon is a cubic
    // Bezier (FUN_10048090) subdivided with random bends (FUN_100f1900) and roughened by 64 stored offsets.
    struct Ribbon {
        bool on = false;
        float life = 0, age = 0;        // +0xb1 / +0xb2 (relative to the slot)
        float step = 0.8f, width = 0.6f, amp = 5.f;  // +0xdc points spacing, +0xdd half-width, +0xde bend
        int depth = 3;                  // +0xdf subdivision levels
        uint8_t seed = 0;               // +0xe0 offset into the 64 roughening values, +2 each draw
        Vec3 cp[4], perp;               // control points (bolt space), the bend / roughening direction
    };
    struct Bolt {
        Vec3 at;                        // strike point (camera height)
        float life = 0.5f, age = 0;     // +0x1a8 (0.2 .. 0.6 s, at least 0.1) / +0x1b0
        float jag[64];                  // +0x1b8: (2 rand - 32767) x 9.1556e-6, about +-0.3 m
        Ribbon rib[4];
    };
    std::vector<Bolt> bolts_;
    void strikeBolt(const Vec3& at);
    void respawnRibbon(Bolt& b, int i);
    void ribbonPoints(const Ribbon& r, const Bolt& b, std::vector<Vec3>& out) const;
};

}  // namespace hg2
