// Collision volumes for static models.
//
// The .M models carry no collision hulls (only per-mesh render BSPs, FUN_1000dd00), so the volumes are derived from
// the drawn geometry: the triangles are sampled (skipping texels the alpha test cuts away), the samples are binned
// into a grid of vertical columns over the model's footprint, each column keeps the height ranges that really hold
// geometry (so gears pass under canopies, arches, bridge decks and tree crowns), roofed spaces closed in by walls are
// filled, and neighbouring columns with matching ranges are merged into boxes.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include <memory>
#include <unordered_map>

#include "assets.h"
#include "../core/fs.h"

namespace hg2 {

struct CollBox { Vec3 lo, hi; };
struct CollShape {
    std::vector<CollBox> boxes;
    Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};  // bounds of all boxes (cheap reject)
    // Boxes come in clusters with their own bounds (one per model, or one per mesh of a huge assembly), so a query
    // only walks the boxes near it.
    struct Cluster { Vec3 lo, hi; uint32_t first, count; };
    std::vector<Cluster> clusters;
    void addCluster(const std::vector<CollBox>& bs);
    // Calls fn(box) for every box whose cluster overlaps the model-space bounds [qlo, qhi].
    template <class F> void query(const Vec3& qlo, const Vec3& qhi, F&& fn) const {
        if (qhi.x < lo.x || qlo.x > hi.x || qhi.y < lo.y || qlo.y > hi.y || qhi.z < lo.z || qlo.z > hi.z) return;
        for (auto& c : clusters) {
            if (qhi.x < c.lo.x || qlo.x > c.hi.x || qhi.y < c.lo.y || qlo.y > c.hi.y || qhi.z < c.lo.z || qlo.z > c.hi.z) continue;
            for (uint32_t k = c.first; k < c.first + c.count; k++) fn(boxes[k]);
        }
    }
};
struct CollModel {
    CollShape full;
    // Only what rises from the model's base (a tree's trunk, not the branches and crown above and around it): the
    // volume Tree_Object instances collide with.
    CollShape trunk;
    // audit figures
    Vec3 meshLo{1e9f, 1e9f, 1e9f}, meshHi{-1e9f, -1e9f, -1e9f};  // all geometry
    float cell = 0;          // column size (m)
    int samples = 0, transparentSamples = 0, sliverTris = 0, decalMeshes = 0;
    float solidArea = 0;     // footprint of columns that block a gear standing at the model's base (m^2)
};

// Opaque test for a sample: material index (0-based, -1 none) and texture coordinates. nullptr: all opaque.
using OpacityFn = std::function<bool(int material, float u, float v)>;

// Alpha masks of the textures a model's materials name (resolved like Scene::texture: .ppm -> .TGA/.BMP), cached.
class TextureAlpha {
public:
    explicit TextureAlpha(const FileSystem& fs) : fs_(fs) {}
    OpacityFn forModel(const Model& m);  // samples with alpha < 0.5 (the renderer's alpha test) are not opaque

private:
    const Image* image(const std::string& name);  // nullptr: no alpha channel / not found
    const FileSystem& fs_;
    std::unordered_map<std::string, std::unique_ptr<Image>> cache_;
};

void buildCollision(const Model& m, const OpacityFn& opaque, CollModel& out);
// The previous scheme (one box per frame mesh, trees split in two), kept for the audit's before/after figures.
void legacyCollision(const Model& m, CollModel& out);

}  // namespace hg2
