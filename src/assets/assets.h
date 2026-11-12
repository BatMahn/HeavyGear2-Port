// Asset decoders: images, .M models, .MRM terrain.
#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

#include "../core/fs.h"
#include "../core/math.h"

namespace hg2 {

struct Image {
    int w = 0, h = 0;
    std::vector<uint8_t> rgba;  // top-down RGBA8
    bool hasAlpha = false;
};
bool decodeImage(const Bytes& data, const std::string& name, Image& out);  // BMP (8/24/32), TGA (24/32, RLE)

struct Material {
    float params[8] = {};  // [1]=specular power? [2..4] specular rgb? [5..7] diffuse rgb (observed 0.7)
    uint32_t flags = 0;
    std::string name, texture;
};

struct Corner { uint32_t v, n, uv; };
struct Poly {
    uint32_t id = 0;
    float plane[4] = {};
    std::vector<Corner> corners;
    int material = -1;  // 0-based, -1 none
};
struct Mesh {
    std::string name;
    std::vector<std::array<float, 2>> uv;
    std::vector<Vec3> v, n;
    std::vector<Poly> polys;
};
struct Frame {
    std::string name;
    uint32_t type = 0;
    double m[12] = {};  // rows X,Y,Z,T after orthonormalisation (engine behaviour)
    int child = -1, sibling = -1;  // 0-based
    int mesh = -1;
};
struct Model {
    std::vector<Material> materials;
    std::vector<Frame> frames;
    std::vector<Mesh> meshes;
};
bool loadModel(const Bytes& data, Model& out);

// .XB animation sets (FUN_10013a70 / FUN_100143b0): per-frame key tracks, D3D .X style.
struct AnimKey { int32_t time; float v[4]; };
struct AnimTrack {
    std::string frame;                       // frame name (case-insensitive match)
    std::vector<AnimKey> rot, scale, pos;    // rot = quaternion (w,x,y,z), pos = translation
};
struct AnimSet {
    std::vector<AnimTrack> tracks;
    int32_t length = 0;                      // last key time
};
bool loadAnimSets(const Bytes& data, std::vector<AnimSet>& out);
// Local matrix for a frame at time t (falls back to the frame's own matrix when untracked).
Mat4 animLocal(const AnimTrack* tr, const Mat4& base, float t);

struct TerrainTile {
    int32_t x0, z0, x1, z1;
    int32_t f[21];       // raw fields (see docs/FORMATS.md); f[12]/f[13] grid dims, f[15] step
    int dim = 3;         // dim x dim height samples (3 or 5 observed)
    int step = 16;       // world units between samples
    std::vector<uint16_t> h;     // heights *0.01, x-major: h[ix*dim + iz]
    std::vector<uint16_t> attr;
    int32_t tex[4];
    float heightAt(int lx, int lz) const;  // local world offset inside the tile (0..32)
};
struct Terrain {
    std::vector<std::string> textures;
    std::vector<TerrainTile> tiles;  // 128*128, row = z
    std::vector<Vec3> table;         // trailing 256 vectors
    float height(int gx, int gz) const;  // 16-unit grid vertex (0..256)
    float sample(float x, float z) const;  // bilinear world height using each tile's own grid
    // Surface type (index into the mission's Surfaces table) at the nearest height sample: bits 8..10 of the
    // sample's attribute word (FUN_100974e0).
    int surfaceAt(float x, float z) const;
};
bool loadTerrain(const Bytes& data, Terrain& out);

}  // namespace hg2
