// Polygon collision for models, as the original tests it.
//
// HG2 has no collision hulls: the world collision pass (FUN_100a0b00) hands each pair of overlapping proxies to the
// proxy's narrow test (FUN_100a63d0), which runs the other unit's probe segments (gear +0x2b0 / +0x370, count +0x430)
// through every mesh frame of this object (frame vtable +0x84 = FUN_100214c0). A mesh with a render BSP is searched
// with FUN_1000d690: a node's plane (its first polygon's stored plane, n.p + d) splits the segment, only a crossing
// from the front side to the back (dir . n < 0) counts, the crossing point must lie inside one of the node's
// polygons (FUN_1000d9c0, no texture alpha test) and each hit shortens the segment, so the nearest crossing wins.
// CollMesh holds the same polygons (fanned into triangles, keeping each polygon's stored plane) in a bounding volume
// tree instead of the file's BSP; the answer - the nearest front-face crossing - is the same.
#pragma once
#include <cstdint>
#include <vector>

#include "assets.h"

namespace hg2 {

struct CollTri {
    Vec3 a, b, c;
    Vec3 n;      // the polygon's stored plane normal
    float d = 0; // plane: n.p + d = 0
};

struct CollHit {
    float t = 2.f;  // fraction along the segment
    Vec3 n;         // plane normal of the polygon hit (same space as the segment)
    int tri = -1;
};

struct CollMesh {
    std::vector<CollTri> tris;
    struct Node { Vec3 lo, hi; int left = -1, right = -1; uint32_t first = 0, count = 0; };
    std::vector<Node> nodes;
    Vec3 lo{1e9f, 1e9f, 1e9f}, hi{-1e9f, -1e9f, -1e9f};  // bounds of all polygons
    bool empty() const { return tris.empty(); }
    void build();  // the tree, after tris are filled
    // Nearest front-face crossing of a -> b with t below hit.t (FUN_1000d690); updates hit and returns true if found.
    bool segment(const Vec3& a, const Vec3& b, CollHit& hit) const;
};

// The polygons of a model in model space (frame transforms at rest), or of one frame's mesh in its own space.
void buildCollMesh(const Model& m, CollMesh& out);
void buildFrameCollMesh(const Model& m, int frame, CollMesh& out);

}  // namespace hg2
