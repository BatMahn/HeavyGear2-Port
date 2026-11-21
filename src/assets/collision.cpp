#include "collision.h"

#include <algorithm>
#include <cmath>
#include <functional>

namespace hg2 {

namespace {
Mat4 frameMatrix(const Frame& f) {
    Mat4 m = Mat4::identity();
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 3; c++) m.m[r][c] = float(f.m[r * 3 + c]);
    return m;
}

void grow(Vec3& lo, Vec3& hi, const Vec3& q) {
    lo = Vec3(std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z));
    hi = Vec3(std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z));
}

// A mesh's polygons, placed by w (a rigid frame transform), fanned into triangles that keep the polygon's plane.
void addMesh(const Mesh& me, const Mat4& w, std::vector<CollTri>& out) {
    for (const Poly& p : me.polys) {
        if (p.corners.size() < 3) continue;
        Vec3 n(p.plane[0], p.plane[1], p.plane[2]);
        if (dot(n, n) < 1e-12f) continue;
        n = normalize(w.transformDir(n));
        auto vert = [&](const Corner& c) { return w.transformPoint(c.v < me.v.size() ? me.v[c.v] : Vec3()); };
        Vec3 v0 = vert(p.corners[0]);
        float d = -dot(n, v0);
        for (size_t k = 1; k + 1 < p.corners.size(); k++) {
            CollTri t;
            t.a = v0, t.b = vert(p.corners[k]), t.c = vert(p.corners[k + 1]);
            t.n = n, t.d = d;
            Vec3 e = cross(t.b - t.a, t.c - t.a);
            if (dot(e, e) < 1e-12f) continue;  // degenerate sliver
            out.push_back(t);
        }
    }
}

bool boxSeg(const Vec3& lo, const Vec3& hi, const Vec3& a, const Vec3& inv, float tmax) {
    float t0 = 0, t1 = tmax;
    const float o[3] = {a.x, a.y, a.z}, iv[3] = {inv.x, inv.y, inv.z}, l[3] = {lo.x, lo.y, lo.z}, h[3] = {hi.x, hi.y, hi.z};
    for (int k = 0; k < 3; k++) {
        float u0 = (l[k] - o[k]) * iv[k], u1 = (h[k] - o[k]) * iv[k];
        if (std::isnan(u0) || std::isnan(u1)) {  // parallel and on a slab boundary
            if (o[k] < l[k] || o[k] > h[k]) return false;
            continue;
        }
        if (u0 > u1) std::swap(u0, u1);
        t0 = std::max(t0, u0), t1 = std::min(t1, u1);
        if (t0 > t1) return false;
    }
    return true;
}
}  // namespace

void buildCollMesh(const Model& m, CollMesh& out) {
    out = CollMesh();
    std::function<void(int, const Mat4&)> walk = [&](int fi, const Mat4& parent) {
        while (fi >= 0 && fi < int(m.frames.size())) {
            const Frame& f = m.frames[size_t(fi)];
            Mat4 w = frameMatrix(f) * parent;
            if (f.mesh >= 0 && size_t(f.mesh) < m.meshes.size()) addMesh(m.meshes[size_t(f.mesh)], w, out.tris);
            if (f.child >= 0) walk(f.child, w);
            fi = f.sibling;
        }
    };
    walk(0, Mat4::identity());
    out.build();
}

void buildFrameCollMesh(const Model& m, int frame, CollMesh& out) {
    out = CollMesh();
    if (frame >= 0 && size_t(frame) < m.frames.size()) {
        const Frame& f = m.frames[size_t(frame)];
        if (f.mesh >= 0 && size_t(f.mesh) < m.meshes.size()) addMesh(m.meshes[size_t(f.mesh)], Mat4::identity(), out.tris);
    }
    out.build();
}

void CollMesh::build() {
    nodes.clear();
    lo = Vec3(1e9f, 1e9f, 1e9f), hi = Vec3(-1e9f, -1e9f, -1e9f);
    if (tris.empty()) return;
    std::vector<Vec3> cen(tris.size());
    for (size_t i = 0; i < tris.size(); i++) cen[i] = (tris[i].a + tris[i].b + tris[i].c) * (1.f / 3.f);
    std::vector<uint32_t> idx(tris.size());
    for (size_t i = 0; i < idx.size(); i++) idx[i] = uint32_t(i);
    std::function<int(uint32_t, uint32_t)> make = [&](uint32_t first, uint32_t count) {
        Node nd;
        for (uint32_t k = first; k < first + count; k++) {
            const CollTri& t = tris[idx[k]];
            grow(nd.lo, nd.hi, t.a), grow(nd.lo, nd.hi, t.b), grow(nd.lo, nd.hi, t.c);
        }
        int self = int(nodes.size());
        nodes.push_back(nd);
        if (count <= 6) {
            nodes[size_t(self)].first = first, nodes[size_t(self)].count = count;
            return self;
        }
        Vec3 e = nd.hi - nd.lo;
        int axis = e.x > e.y && e.x > e.z ? 0 : e.y > e.z ? 1 : 2;
        auto key = [&](uint32_t i) { return axis == 0 ? cen[i].x : axis == 1 ? cen[i].y : cen[i].z; };
        uint32_t mid = first + count / 2;
        std::nth_element(idx.begin() + first, idx.begin() + mid, idx.begin() + first + count,
                         [&](uint32_t p, uint32_t q) { return key(p) < key(q); });
        int l = make(first, mid - first);
        int r = make(mid, first + count - mid);
        nodes[size_t(self)].left = l, nodes[size_t(self)].right = r;
        return self;
    };
    make(0, uint32_t(tris.size()));
    std::vector<CollTri> sorted(tris.size());
    for (size_t i = 0; i < idx.size(); i++) sorted[i] = tris[idx[i]];
    tris.swap(sorted);
    lo = nodes[0].lo, hi = nodes[0].hi;
}

bool CollMesh::segment(const Vec3& a, const Vec3& b, CollHit& hit) const {
    if (nodes.empty()) return false;
    Vec3 dir = b - a;
    Vec3 inv(1.f / dir.x, 1.f / dir.y, 1.f / dir.z);
    bool found = false;
    int stack[64], sp = 0;
    stack[sp++] = 0;
    while (sp > 0) {
        const Node& nd = nodes[size_t(stack[--sp])];
        if (!boxSeg(nd.lo, nd.hi, a, inv, std::min(1.f, hit.t))) continue;
        if (nd.left >= 0) {
            if (sp < 62) stack[sp++] = nd.left, stack[sp++] = nd.right;
            continue;
        }
        for (uint32_t k = nd.first; k < nd.first + nd.count; k++) {
            const CollTri& tr = tris[k];
            // FUN_1000d690: the start on the front of the plane, the end behind it, moving against the normal
            float da = dot(tr.n, a) + tr.d, dn = dot(tr.n, dir);
            if (da < 0 || dn >= 0) continue;
            float t = -da / dn;
            if (t < 0 || t > 1 || t >= hit.t) continue;
            Vec3 p = a + dir * t;
            // inside the polygon (FUN_1000d9c0)
            Vec3 e0 = tr.b - tr.a, e1 = tr.c - tr.b, e2 = tr.a - tr.c;
            Vec3 c0 = cross(e0, p - tr.a), c1 = cross(e1, p - tr.b), c2 = cross(e2, p - tr.c);
            Vec3 fn = cross(e0, tr.c - tr.a);  // the triangle's winding normal (the stored plane may face either way)
            float s0 = dot(c0, fn), s1 = dot(c1, fn), s2 = dot(c2, fn);
            const float eps = -1e-6f * dot(fn, fn);
            if (s0 < eps || s1 < eps || s2 < eps) continue;
            hit.t = t, hit.n = tr.n, hit.tri = int(k);
            found = true;
        }
    }
    return found;
}

}  // namespace hg2
