#include "collision.h"

#include <algorithm>
#include <cmath>
#include <map>

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

// Every frame mesh with its model-space transform.
void forMeshes(const Model& m, const std::function<void(const Mesh&, const Mat4&)>& fn) {
    std::function<void(int, const Mat4&)> walk = [&](int fi, const Mat4& parent) {
        while (fi >= 0 && fi < int(m.frames.size())) {
            const Frame& f = m.frames[size_t(fi)];
            Mat4 w = frameMatrix(f) * parent;
            if (f.mesh >= 0 && size_t(f.mesh) < m.meshes.size() && !m.meshes[size_t(f.mesh)].v.empty()) fn(m.meshes[size_t(f.mesh)], w);
            if (f.child >= 0) walk(f.child, w);
            fi = f.sibling;
        }
    };
    walk(0, Mat4::identity());
}

// Flat pieces the gears walk over (decals, pads) and terrain-sized meshes take no part in collision.
bool collidable(const Vec3& lo, const Vec3& hi) {
    float hgt = hi.y - lo.y, area = (hi.x - lo.x) * (hi.z - lo.z);
    return hgt > 0.8f && area < 250000.f;
}

constexpr float kStep = 1.2f, kGearHeight = 7.f;  // what Scene::collide steps over / a gear's height
}  // namespace

const Image* TextureAlpha::image(const std::string& name) {
    if (name.empty()) return nullptr;
    std::string key = upper(name);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second.get();
    std::unique_ptr<Image> img;
    std::string base = key.substr(0, key.rfind('.'));
    for (const char* ext : {".TGA", ".BMP", ""}) {
        std::string cand = *ext ? base + ext : key;
        Bytes b;
        auto im = std::make_unique<Image>();
        if (fs_.read(cand, b) && decodeImage(b, cand, *im)) {
            if (im->hasAlpha && im->w > 0 && im->h > 0) img = std::move(im);
            break;
        }
    }
    const Image* p = img.get();
    cache_[key] = std::move(img);
    return p;
}

OpacityFn TextureAlpha::forModel(const Model& m) {
    std::vector<const Image*> imgs;
    bool any = false;
    for (auto& mat : m.materials) imgs.push_back(image(mat.texture)), any = any || imgs.back();
    if (!any) return nullptr;
    return [imgs](int mat, float u, float v) {
        if (mat < 0 || size_t(mat) >= imgs.size() || !imgs[size_t(mat)]) return true;
        const Image& im = *imgs[size_t(mat)];
        u -= std::floor(u), v -= std::floor(v);
        int x = std::min(im.w - 1, int(u * float(im.w))), y = std::min(im.h - 1, int(v * float(im.h)));
        return im.rgba[(size_t(y) * size_t(im.w) + size_t(x)) * 4 + 3] >= 128;
    };
}

void CollShape::addCluster(const std::vector<CollBox>& bs) {
    if (bs.empty()) return;
    Cluster c{Vec3(1e9f, 1e9f, 1e9f), Vec3(-1e9f, -1e9f, -1e9f), uint32_t(boxes.size()), uint32_t(bs.size())};
    for (auto& b : bs) boxes.push_back(b), grow(c.lo, c.hi, b.lo), grow(c.lo, c.hi, b.hi);
    grow(lo, hi, c.lo), grow(lo, hi, c.hi);
    clusters.push_back(c);
}

void legacyCollision(const Model& m, CollModel& out) {
    forMeshes(m, [&](const Mesh& me, const Mat4& w) {
        CollBox b{Vec3(1e9f, 1e9f, 1e9f), Vec3(-1e9f, -1e9f, -1e9f)};
        for (auto& p : me.v) grow(b.lo, b.hi, w.transformPoint(p));
        grow(out.meshLo, out.meshHi, b.lo), grow(out.meshLo, out.meshHi, b.hi);
        if (!collidable(b.lo, b.hi)) return;
        float hgt = b.hi.y - b.lo.y, area = (b.hi.x - b.lo.x) * (b.hi.z - b.lo.z);
        CollBox lo{Vec3(1e9f, 1e9f, 1e9f), Vec3(-1e9f, -1e9f, -1e9f)}, up = lo;
        float cut = b.lo.y + 0.25f * hgt;
        for (auto& p : me.v) {
            Vec3 q = w.transformPoint(p);
            CollBox& t = q.y < cut ? lo : up;
            grow(t.lo, t.hi, q);
        }
        float loArea = (lo.hi.x - lo.lo.x) * (lo.hi.z - lo.lo.z);
        if (hgt > 10.f && lo.hi.x > lo.lo.x && up.hi.x > up.lo.x && loArea < 0.3f * area) {
            lo.hi.y = std::max(lo.hi.y, up.lo.y);
            out.full.addCluster({lo, up});
        } else {
            out.full.addCluster({b});
        }
    });
    out.trunk = out.full;
}

namespace {
struct MeshRef {
    const Mesh* mesh;
    std::vector<Vec3> v;  // model-space vertices
    Vec3 lo, hi;
};

// Column volumes for a group of meshes sharing one grid. `fill`: close roofed spaces shut in by walls.
void buildGroup(const std::vector<const MeshRef*>& refs, const OpacityFn& opaque, float spacingFloor, bool fill, CollModel& out,
                std::vector<CollBox>& result) {
    Vec3 lo(1e9f, 1e9f, 1e9f), hi(-1e9f, -1e9f, -1e9f);
    for (auto* r : refs) grow(lo, hi, r->lo), grow(lo, hi, r->hi);
    float ext = std::max(hi.x - lo.x, hi.z - lo.z);
    const float cell = std::max(1.f, ext / 48.f);
    const int nx = std::max(1, int(std::ceil((hi.x - lo.x) / cell + 1e-4f))), nz = std::max(1, int(std::ceil((hi.z - lo.z) / cell + 1e-4f)));
    out.cell = std::max(out.cell, cell);
    // sample spacing: half a column, at most 0.5 m - coarser for huge models so the work stays bounded
    const float spacing = std::max(std::min(0.5f, cell * 0.5f), std::min(cell * 0.5f, spacingFloor));
    struct Sample { float x, y, z; };
    std::vector<std::vector<Sample>> col(size_t(nx) * size_t(nz));
    auto cellOf = [&](float x, float z) {
        int ix = std::clamp(int((x - lo.x) / cell), 0, nx - 1), iz = std::clamp(int((z - lo.z) / cell), 0, nz - 1);
        return size_t(iz) * size_t(nx) + size_t(ix);
    };

    // sample the triangles' opaque surface into the columns
    for (auto* ref : refs) {
        const Mesh& me = *ref->mesh;
        const auto& wv = ref->v;
        for (const Poly& p : me.polys) {
            if (p.corners.size() < 3) continue;
            for (size_t k = 1; k + 1 < p.corners.size(); k++) {
                const Corner* cs[3] = {&p.corners[0], &p.corners[k], &p.corners[k + 1]};
                Vec3 v[3];
                float uv[3][2] = {};
                bool ok = true;
                for (int j = 0; j < 3; j++) {
                    if (cs[j]->v >= wv.size()) ok = false;
                    else v[j] = wv[cs[j]->v];
                    if (cs[j]->uv < me.uv.size()) uv[j][0] = me.uv[cs[j]->uv][0], uv[j][1] = me.uv[cs[j]->uv][1];
                }
                if (!ok) continue;
                Vec3 e0 = v[1] - v[0], e1 = v[2] - v[0], e2 = v[2] - v[1];
                float area2 = length(cross(e0, e1));
                float longest = std::max({length(e0), length(e1), length(e2)});
                // degenerate (zero-width) triangles: outlines and padding, nothing to stand against
                if (area2 < 0.02f * longest) {
                    out.sliverTris++;
                    continue;
                }
                // a grid along the two edges meeting opposite the longest one (long slivers get a row, not a square)
                int o = length(e2) >= length(e0) && length(e2) >= length(e1) ? 0 : length(e1) >= length(e0) ? 1 : 2;
                int ia = (o + 1) % 3, ib = (o + 2) % 3;
                Vec3 ea = v[ia] - v[o], eb = v[ib] - v[o];
                int na = std::clamp(int(std::ceil(length(ea) / spacing)), 1, 400), nb = std::clamp(int(std::ceil(length(eb) / spacing)), 1, 400);
                for (int i = 0; i <= na; i++)
                    for (int j = 0; j <= nb; j++) {
                        float fa = float(i) / float(na), fb = float(j) / float(nb);
                        if (fa + fb > 1.0001f) break;
                        Vec3 q = v[o] + ea * fa + eb * fb;
                        float w3[3];
                        w3[o] = 1.f - fa - fb, w3[ia] = fa, w3[ib] = fb;
                        out.samples++;
                        if (opaque) {
                            float u = uv[0][0] * w3[0] + uv[1][0] * w3[1] + uv[2][0] * w3[2], t = uv[0][1] * w3[0] + uv[1][1] * w3[1] + uv[2][1] * w3[2];
                            if (!opaque(p.material, u, t)) {
                                out.transparentSamples++;
                                continue;
                            }
                        }
                        col[cellOf(q.x, q.z)].push_back({q.x, q.y, q.z});
                    }
            }
        }
    }

    // columns -> height ranges (with the horizontal extent of the samples in each); ranges closer than a gear's
    // height merge: nothing fits between them, and fewer ranges merge into fewer boxes
    struct Range { float lo, hi, x0, x1, z0, z1; bool used = false; };
    std::vector<std::vector<Range>> ranges(col.size());
    for (size_t c = 0; c < col.size(); c++) {
        auto& s = col[c];
        if (s.empty()) continue;
        std::sort(s.begin(), s.end(), [](const Sample& a, const Sample& b) { return a.y < b.y; });
        Range r{s[0].y, s[0].y, s[0].x, s[0].x, s[0].z, s[0].z};
        for (size_t k = 1; k <= s.size(); k++) {
            if (k == s.size() || s[k].y - r.hi > kGearHeight) {
                ranges[c].push_back(r);
                if (k < s.size()) r = {s[k].y, s[k].y, s[k].x, s[k].x, s[k].z, s[k].z};
                continue;
            }
            r.hi = s[k].y;
            r.x0 = std::min(r.x0, s[k].x), r.x1 = std::max(r.x1, s[k].x), r.z0 = std::min(r.z0, s[k].z), r.z1 = std::max(r.z1, s[k].z);
        }
        s.clear(), s.shrink_to_fit();
    }

    // Roofed space shut in by walls (a building's inside, which has no floor polygons) is filled down to the base, so a
    // gear pushed past a wall cannot end up in a hollow. Open-air yards and the space under arches stay open: anything
    // reachable at ground level from outside the footprint is left alone.
    const float base = lo.y;
    auto blocks = [&](size_t c) {
        for (auto& r : ranges[c])
            if (r.lo < base + kGearHeight && r.hi > base + kStep) return true;
        return false;
    };
    std::vector<uint8_t> state(col.size(), 0);  // 1 blocking, 2 reached from outside, 3 shut in
    for (size_t c = 0; c < col.size(); c++) state[c] = blocks(c) ? 1 : 0;
    if (fill) {
        std::vector<size_t> stack;
        for (int x = 0; x < nx; x++)
            for (int z = 0; z < nz; z++) {
                size_t c = size_t(z) * nx + x;
                if ((x == 0 || z == 0 || x == nx - 1 || z == nz - 1) && state[c] == 0) state[c] = 2, stack.push_back(c);
            }
        while (!stack.empty()) {
            size_t c = stack.back();
            stack.pop_back();
            int x = int(c % size_t(nx)), z = int(c / size_t(nx));
            const int d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
            for (auto& dd : d) {
                int x2 = x + dd[0], z2 = z + dd[1];
                if (x2 < 0 || z2 < 0 || x2 >= nx || z2 >= nz) continue;
                size_t c2 = size_t(z2) * nx + x2;
                if (state[c2] == 0) state[c2] = 2, stack.push_back(c2);
            }
        }
        // Each shut-in pocket is filled only when it looks like a hollow shell: small (a building, not a station deck
        // or hangar the level is played in) and with no floor at the base (walkable insides have floor polygons).
        std::vector<size_t> pocket;
        for (size_t c0 = 0; c0 < col.size(); c0++) {
            if (state[c0] != 0) continue;
            pocket.clear();
            bool floor = false;
            state[c0] = 3, stack.push_back(c0);
            while (!stack.empty()) {
                size_t c = stack.back();
                stack.pop_back();
                pocket.push_back(c);
                for (auto& r : ranges[c]) floor = floor || r.lo < base + kStep;
                int x = int(c % size_t(nx)), z = int(c / size_t(nx));
                const int d[4][2] = {{1, 0}, {-1, 0}, {0, 1}, {0, -1}};
                for (auto& dd : d) {
                    int x2 = x + dd[0], z2 = z + dd[1];
                    if (x2 < 0 || z2 < 0 || x2 >= nx || z2 >= nz) continue;
                    size_t c2 = size_t(z2) * nx + x2;
                    if (state[c2] == 0) state[c2] = 3, stack.push_back(c2);
                }
            }
            if (floor || float(pocket.size()) * cell * cell > 2500.f) continue;
            for (size_t c : pocket) {
                if (ranges[c].empty()) continue;  // open to the sky: a yard, left alone
                float top = base;
                for (auto& r : ranges[c]) top = std::max(top, r.hi);
                float x0 = lo.x + float(c % size_t(nx)) * cell, z0 = lo.z + float(c / size_t(nx)) * cell;
                ranges[c] = {Range{base, top, x0, x0 + cell, z0, z0 + cell}};
                state[c] = 1;
            }
        }
    }
    for (size_t c = 0; c < col.size(); c++)
        if (state[c] == 1) out.solidArea += cell * cell;

    // merge neighbouring columns whose ranges match into boxes (greedy: along x, then whole rows along z). Low edges
    // and tops near the base must stay close (what can be stepped onto or walked under); parts far above the ground
    // (roofs, overhangs) may merge more loosely.
    auto tol = [&](float y) { return std::max(0.4f, 0.25f * (y - base - kGearHeight)); };
    auto match = [&](size_t c, const Range& want) -> Range* {
        for (auto& r : ranges[c])
            if (!r.used && std::fabs(r.lo - want.lo) <= tol(std::min(r.lo, want.lo)) && std::fabs(r.hi - want.hi) <= tol(std::min(r.hi, want.hi))) return &r;
        return nullptr;
    };
    std::vector<CollBox> boxes;
    for (int z = 0; z < nz; z++)
        for (int x = 0; x < nx; x++)
            for (auto& r0 : ranges[size_t(z) * nx + x]) {
                if (r0.used) continue;
                Range seed = r0;
                r0.used = true;
                CollBox b{Vec3(r0.x0, r0.lo, r0.z0), Vec3(r0.x1, r0.hi, r0.z1)};
                int xe = x + 1;
                for (; xe < nx; xe++) {
                    Range* r = match(size_t(z) * nx + xe, seed);
                    if (!r) break;
                    r->used = true;
                    grow(b.lo, b.hi, Vec3(r->x0, r->lo, r->z0)), grow(b.lo, b.hi, Vec3(r->x1, r->hi, r->z1));
                }
                for (int z2 = z + 1; z2 < nz; z2++) {
                    std::vector<Range*> row;
                    for (int x2 = x; x2 < xe; x2++) {
                        Range* r = match(size_t(z2) * nx + x2, seed);
                        if (!r) break;
                        row.push_back(r);
                    }
                    if (int(row.size()) != xe - x) break;
                    for (Range* r : row) r->used = true, grow(b.lo, b.hi, Vec3(r->x0, r->lo, r->z0)), grow(b.lo, b.hi, Vec3(r->x1, r->hi, r->z1));
                }
                boxes.push_back(b);
            }

    // Second pass: neighbouring boxes whose union is hardly bigger than the two (ragged roof edges, leafy crowns) are
    // merged, as long as the union does not reach lower or higher than allowed near the base.
    auto vol = [](const CollBox& b) {
        return std::max(0.05f, b.hi.x - b.lo.x) * std::max(0.05f, b.hi.y - b.lo.y) * std::max(0.05f, b.hi.z - b.lo.z);
    };
    for (bool merged = true; merged;) {
        merged = false;
        for (size_t a = 0; a < boxes.size(); a++)
            for (size_t c = a + 1; c < boxes.size(); c++) {
                const CollBox &A = boxes[a], &C = boxes[c];
                if (std::fabs(A.lo.y - C.lo.y) > tol(std::min(A.lo.y, C.lo.y)) || std::fabs(A.hi.y - C.hi.y) > tol(std::min(A.hi.y, C.hi.y))) continue;
                CollBox u = A;
                grow(u.lo, u.hi, C.lo), grow(u.lo, u.hi, C.hi);
                bool overhead = std::min(A.lo.y, C.lo.y) > base + kGearHeight + 3.f;  // out of reach from the ground
                if (vol(u) > (overhead ? 1.4f : 1.2f) * (vol(A) + vol(C))) continue;
                boxes[a] = u;
                boxes.erase(boxes.begin() + long(c));
                merged = true;
                c = a;  // the grown box may now take others
            }
    }
    for (auto& b : boxes)
        if (b.hi.x - b.lo.x >= 0.1f || b.hi.y - b.lo.y >= 0.1f || b.hi.z - b.lo.z >= 0.1f) result.push_back(b);  // not lone samples
}
}  // namespace

void buildCollision(const Model& m, const OpacityFn& opaque, CollModel& out) {
    std::vector<MeshRef> refs;
    double area = 0;
    forMeshes(m, [&](const Mesh& me, const Mat4& w) {
        MeshRef r{&me, {}, Vec3(1e9f, 1e9f, 1e9f), Vec3(-1e9f, -1e9f, -1e9f)};
        for (auto& p : me.v) r.v.push_back(w.transformPoint(p)), grow(r.lo, r.hi, r.v.back());
        grow(out.meshLo, out.meshHi, r.lo), grow(out.meshLo, out.meshHi, r.hi);
        if (!collidable(r.lo, r.hi)) {
            out.decalMeshes++;
            return;
        }
        for (const Poly& p : me.polys)
            for (size_t k = 1; k + 1 < p.corners.size(); k++)
                if (p.corners[0].v < r.v.size() && p.corners[k].v < r.v.size() && p.corners[k + 1].v < r.v.size())
                    area += 0.5 * double(length(cross(r.v[p.corners[k].v] - r.v[p.corners[0].v], r.v[p.corners[k + 1].v] - r.v[p.corners[0].v])));
        refs.push_back(std::move(r));
    });
    if (refs.empty()) return;
    Vec3 lo(1e9f, 1e9f, 1e9f), hi(-1e9f, -1e9f, -1e9f);
    for (auto& r : refs) grow(lo, hi, r.lo), grow(lo, hi, r.hi);
    const float spacingFloor = float(std::sqrt(area / 2e6));  // keeps a model to roughly two million samples
    auto add = [&](const std::vector<CollBox>& boxes) {
        std::vector<CollBox> trunk;
        for (auto& b : boxes)
            if (b.lo.y <= lo.y + 2.f) trunk.push_back(b);
        // many boxes: split into tiles of about 24 m so a query near one corner skips the rest
        auto addTiled = [&](CollShape& sh, const std::vector<CollBox>& bs) {
            if (bs.size() <= 48) return sh.addCluster(bs);
            std::map<std::pair<int, int>, std::vector<CollBox>> tiles;
            for (auto& b : bs) tiles[{int(std::floor((b.lo.x + b.hi.x) * 0.5f / 24.f)), int(std::floor((b.lo.z + b.hi.z) * 0.5f / 24.f))}].push_back(b);
            for (auto& [k, t] : tiles) sh.addCluster(t);
        };
        addTiled(out.full, boxes), addTiled(out.trunk, trunk);
    };
    if (std::max(hi.x - lo.x, hi.z - lo.z) <= 200.f) {
        std::vector<const MeshRef*> all;
        for (auto& r : refs) all.push_back(&r);
        std::vector<CollBox> boxes;
        buildGroup(all, opaque, spacingFloor, true, out, boxes);
        add(boxes);
    } else {
        // Huge assemblies (Gomorrah's city blocks, a whole base in one model) get a grid and a cluster per mesh: one
        // grid over kilometres would make every column tens of metres wide, and a level is walked inside them.
        for (auto& r : refs) {
            std::vector<CollBox> boxes;
            buildGroup({&r}, opaque, spacingFloor, false, out, boxes);
            add(boxes);
        }
    }
}

}  // namespace hg2
