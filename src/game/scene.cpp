#include "scene.h"

#include <algorithm>
#include <cstring>
#include <SDL3/SDL.h>

#include <cmath>
#include <functional>
#include <map>

namespace hg2 {

namespace {
// Geometry name as a file: '@' prefixes (shared geometry) dropped; names whose base is longer than the 8.3 file names
// ("pAbpg20005.m", "pAmrd30005.m" in S25 / S27 / S37 / H07 / H09) fall back to the last 8 characters of the base
// (BPG20005.M, MRD30005.M), which is the file the game ships.
std::string modelKey(const FileSystem& fs, const std::string& name) {
    std::string key = upper(name);
    while (!key.empty() && key[0] == '@') key.erase(0, 1);
    size_t dot = key.rfind('.');
    if (dot != std::string::npos && dot > 8 && !fs.exists(key)) {
        std::string alt = key.substr(dot - 8);
        if (fs.exists(alt)) return alt;
    }
    return key;
}
constexpr float kDeg = 3.14159265358979f / 180.f;

Mat4 frameMatrix(const Frame& f) {
    Mat4 m = Mat4::identity();
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 3; c++) m.m[r][c] = float(f.m[r * 3 + c]);
    return m;
}
}  // namespace

int Scene::texture(const std::string& name) {
    if (name.empty()) return r_.whiteTexture();
    std::string key = upper(name);
    auto it = textures_.find(key);
    if (it != textures_.end()) return it->second;
    // .ppm references resolve to a .TGA/.BMP with the same base name
    std::string base = key.substr(0, key.rfind('.'));
    int id = r_.whiteTexture();
    for (const char* ext : {".TGA", ".BMP", ""}) {
        std::string cand = *ext ? base + ext : key;
        Bytes b;
        Image img;
        if (fs_.read(cand, b) && decodeImage(b, cand, img)) {
            id = r_.createTexture(img);
            break;
        }
    }
    if (id == r_.whiteTexture()) SDL_Log("texture not found: %s", name.c_str());
    textures_[key] = id;
    return id;
}

// onlyFrame >= 0: emit just that frame's mesh in its local space (for rigged models).
void Scene::appendModel(const Model& m, std::vector<Vertex>& out, std::vector<DrawBatch>& batches, int onlyFrame) {
    std::map<int, std::vector<Vertex>> byTex;
    std::vector<int> matTex(m.materials.size());
    for (size_t i = 0; i < m.materials.size(); i++) matTex[i] = texture(m.materials[i].texture);
    std::function<void(int, const Mat4&)> walk = [&](int fi, const Mat4& parent) {
        while (fi >= 0 && fi < int(m.frames.size())) {
            const Frame& f = m.frames[size_t(fi)];
            Mat4 w = onlyFrame >= 0 ? Mat4::identity() : frameMatrix(f) * parent;
            if (f.mesh >= 0 && (onlyFrame < 0 || onlyFrame == fi)) {
                const Mesh& me = m.meshes[size_t(f.mesh)];
                for (const Poly& p : me.polys) {
                    if (p.corners.size() < 3) continue;
                    int tex = p.material >= 0 && size_t(p.material) < matTex.size() ? matTex[size_t(p.material)]
                                                                                    : r_.whiteTexture();
                    float shade = 1.f;
                    if (p.material >= 0 && size_t(p.material) < m.materials.size()) {
                        const float* c = m.materials[size_t(p.material)].params;
                        shade = std::max(0.3f, std::min(1.f, (c[5] + c[6] + c[7]) / 3.f / 0.7f));
                    }
                    auto vert = [&](const Corner& c) {
                        Vertex v{};
                        Vec3 pos = c.v < me.v.size() ? me.v[c.v] : Vec3();
                        Vec3 n = c.n < me.n.size() ? me.n[c.n] : Vec3(0, 1, 0);
                        pos = w.transformPoint(pos);
                        n = normalize(w.transformDir(n));
                        v.pos[0] = pos.x, v.pos[1] = pos.y, v.pos[2] = pos.z;
                        v.normal[0] = n.x, v.normal[1] = n.y, v.normal[2] = n.z;
                        if (c.uv < me.uv.size()) v.uv[0] = me.uv[c.uv][0], v.uv[1] = me.uv[c.uv][1];
                        uint8_t s = uint8_t(255 * shade);
                        v.color[0] = v.color[1] = v.color[2] = s;
                        v.color[3] = 255;
                        return v;
                    };
                    auto& dst = byTex[tex];
                    for (size_t k = 1; k + 1 < p.corners.size(); k++) {
                        dst.push_back(vert(p.corners[0]));
                        dst.push_back(vert(p.corners[k]));
                        dst.push_back(vert(p.corners[k + 1]));
                    }
                }
            }
            if (onlyFrame >= 0) break;
            if (f.child >= 0) walk(f.child, w);
            fi = f.sibling;
        }
    };
    walk(onlyFrame >= 0 ? onlyFrame : 0, Mat4::identity());
    for (auto& [tex, vs] : byTex) {
        DrawBatch b;
        b.texture = tex;
        b.first = uint32_t(out.size());
        b.count = uint32_t(vs.size());
        b.alphaTest = true;
        out.insert(out.end(), vs.begin(), vs.end());
        batches.push_back(b);
    }
}

GpuMesh* Scene::model(const std::string& name) {
    std::string key = modelKey(fs_, name);  // '@' marks a shared/instanced geometry reference
    auto it = models_.find(key);
    if (it != models_.end()) return it->second;
    GpuMesh* mesh = nullptr;
    Bytes b;
    Model m;
    if (fs_.read(key, b) && loadModel(b, m)) {
        std::vector<Vertex> v;
        std::vector<DrawBatch> batches;
        appendModel(m, v, batches);
        mesh = r_.createMesh(v, std::move(batches));
        // the polygons the collision pass tests (collision.h)
        auto cm = std::make_unique<CollMesh>();
        buildCollMesh(m, *cm);
        coll_[key] = std::move(cm);
    } else {
        SDL_Log("model not found: %s", name.c_str());
    }
    models_[key] = mesh;
    return mesh;
}

Scene::Rigged* Scene::rigged(const std::string& name) {
    std::string key = modelKey(fs_, name);
    auto it = rigged_.find(key);
    if (it != rigged_.end()) return it->second.get();
    auto rg = std::make_unique<Rigged>();
    Bytes b;
    if (!fs_.read(key, b) || !loadModel(b, rg->model)) {
        rigged_[key] = nullptr;
        return nullptr;
    }
    for (size_t i = 0; i < rg->model.frames.size(); i++) {
        std::vector<Vertex> v;
        std::vector<DrawBatch> batches;
        if (rg->model.frames[i].mesh >= 0) appendModel(rg->model, v, batches, int(i));
        rg->parts.push_back(v.empty() ? nullptr : r_.createMesh(v, std::move(batches)));
        std::unique_ptr<CollMesh> cm;
        if (rg->model.frames[i].mesh >= 0) {
            cm = std::make_unique<CollMesh>();
            buildFrameCollMesh(rg->model, int(i), *cm);
            if (cm->empty()) cm.reset();
        }
        rg->coll.push_back(std::move(cm));
    }
    {
        // FUN_10017ff0: the bounding sphere at rest - the box of the whole hierarchy (FUN_10018af0) gives the centre,
        // the farthest vertex from it the radius (FUN_10018f80)
        CollMesh all;
        buildCollMesh(rg->model, all);
        if (!all.empty()) {
            rg->centre = (all.lo + all.hi) * 0.5f;
            float r2 = 0;
            for (auto& t : all.tris)
                for (const Vec3* q : {&t.a, &t.b, &t.c}) r2 = std::max(r2, dot(*q - rg->centre, *q - rg->centre));
            rg->radius = std::sqrt(r2);
        }
        // FUN_100a79b0: with no centre frame named by the owner (+0x158), the middle one of the mesh frames
        std::vector<int> meshFrames;
        for (size_t i = 0; i < rg->model.frames.size(); i++)
            if (rg->coll[i]) meshFrames.push_back(int(i));
        if (!meshFrames.empty()) rg->middle = meshFrames[meshFrames.size() / 2];
    }
    Rigged* p = rg.get();
    rigged_[key] = std::move(rg);
    return p;
}

const std::vector<AnimSet>* Scene::animSets(const std::string& ecyName) {
    std::string key = upper(ecyName);
    auto it = anims_.find(key);
    if (it != anims_.end()) return it->second.get();
    std::unique_ptr<std::vector<AnimSet>> sets;
    Bytes ecy, xb;
    if (fs_.read(key, ecy) && ecy.size() > 16) {
        Reader r(ecy);
        r.skip(12);
        std::string xbName = r.str();  // ECY header names its .xb animation library
        if (fs_.read(xbName, xb)) {
            sets = std::make_unique<std::vector<AnimSet>>();
            if (!loadAnimSets(xb, *sets)) sets.reset();
        }
    }
    if (!sets) SDL_Log("animations not found for %s", ecyName.c_str());
    auto* p = sets.get();
    anims_[key] = std::move(sets);
    return p;
}

namespace {
// frame names in the models carry a "frm-" prefix; damage tables and scripts use the bare part name
bool frameIs(const std::string& frameName, const std::string& want) {
    std::string a = lower(frameName), b = lower(want);
    return a == b || (a.size() == b.size() + 4 && a.compare(0, 4, "frm-") == 0 && a.compare(4, std::string::npos, b) == 0);
}
}  // namespace

void Scene::walkPose(const Instance& i, const std::function<void(int, const Frame&, const Mat4&, bool)>& fn) const {
    if (!i.rig) return;
    const Model& m = i.rig->model;
    const AnimSet* set = i.sets && size_t(i.set) < i.sets->size() ? &(*i.sets)[size_t(i.set)] : nullptr;
    float t = i.frame >= 0 ? i.frame : time_ * 30.f;
    if (i.frame < 0 && set && set->length > 0) t = std::fmod(t, float(set->length));
    const AnimSet* fade = i.fadeSet >= 0 && i.fadeWeight > 0 && i.sets && size_t(i.fadeSet) < i.sets->size() ? &(*i.sets)[size_t(i.fadeSet)] : nullptr;
    const AnimSet* over = i.overlaySet >= 0 && i.sets && size_t(i.overlaySet) < i.sets->size() ? &(*i.sets)[size_t(i.overlaySet)] : nullptr;
    // Locomotion sets carry root motion: the top frame walks forward through the cycle (7.5 m over a Jaguar's walk) and
    // jumps back when it loops. As in FUN_10064d20 the top frames' horizontal travel is taken out here (its height -
    // the step's bob - stays) and Player moves the body by it instead (animTravel), playing the cycle at speed / stride.
    std::function<void(int, const Mat4&, bool, bool, int)> walk = [&](int fi, const Mat4& parent, bool hiddenBranch, bool inOverlay, int depth) {
        bool top = depth <= 1;  // the model's root and its children (the body, frm-bda, carries the travel)
        while (fi >= 0 && fi < int(m.frames.size())) {
            const Frame& f = m.frames[size_t(fi)];
            const AnimTrack* tr = nullptr;
            bool ov = inOverlay || (over && frameIs(f.name, i.overlayRoot));
            float ft = t;
            if (ov && over) {
                std::string n = lower(f.name);
                for (auto& trk : over->tracks)
                    if (trk.frame == n) {
                        tr = &trk, ft = i.overlayFrame;
                        break;
                    }
            }
            if (!tr && set) {
                std::string n = lower(f.name);
                for (auto& trk : set->tracks)
                    if (trk.frame == n) {
                        tr = &trk;
                        break;
                    }
            }
            Mat4 local = animLocal(tr, frameMatrix(f), ft);
            if (top && tr && !tr->pos.empty()) {
                Mat4 l0 = animLocal(tr, frameMatrix(f), 0.f);
                local.m[3][0] = l0.m[3][0], local.m[3][2] = l0.m[3][2];
            }
            if (fade && !(ov && over)) {  // cross-fade from the previous cycle
                const AnimTrack* ftr = nullptr;
                std::string n = lower(f.name);
                for (auto& trk : fade->tracks)
                    if (trk.frame == n) {
                        ftr = &trk;
                        break;
                    }
                if (ftr || tr) local = animLocalBlend(ftr, i.fadeFrame, tr, ft, frameMatrix(f), 1.f - i.fadeWeight, top);
            }
            if (i.twist != 0 && frameIs(f.name, "tst")) local = Mat4::rotY(i.twist) * local;
            if (i.pitch != 0 && frameIs(f.name, "xa")) local = Mat4::rotX(i.pitch) * local;  // rotX(+) lifts -Z
            Mat4 w = local * parent;
            bool hid = hiddenBranch;
            for (auto& h : i.hiddenFrames) hid = hid || frameIs(f.name, h);
            fn(fi, f, w, hid);
            if (f.child >= 0) walk(f.child, w, hid, ov, depth + 1);
            fi = f.sibling;
        }
    };
    walk(0, i.world, false, false, 0);
}

bool Scene::objectWorld(int object, Mat4& out) const {
    for (auto& i : instances_)
        if (i.object == object) {
            out = i.world;
            return true;
        }
    return false;
}

bool Scene::objectPosition(int object, Vec3& out) const {
    for (auto& i : instances_)
        if (i.object == object) {
            out = Vec3(i.world.m[3][0], i.world.m[3][1], i.world.m[3][2]);
            return true;
        }
    return false;
}

bool Scene::objectAxis(int object, int row, Vec3& out) const {
    for (auto& i : instances_)
        if (i.object == object) {
            out = Vec3(i.world.m[row][0], i.world.m[row][1], i.world.m[row][2]);
            return true;
        }
    return false;
}

bool Scene::frameWorldMatrix(int object, const std::string& frame, Mat4& out) const {
    for (auto& i : instances_) {
        if (i.object != object || !i.rig) continue;
        bool found = false;
        walkPose(i, [&](int, const Frame& f, const Mat4& w, bool) {
            if (!found && frameIs(f.name, frame)) out = w, found = true;
        });
        return found;
    }
    return false;
}

const Scene::GenGraph* Scene::walkGraph(int object) {
    auto it = genGraphs_.find(object);
    if (it != genGraphs_.end()) return it->second.get();
    std::unique_ptr<GenGraph>& g = genGraphs_[object];
    const auto& objs = mission_.objects;
    if (object < 0 || size_t(object) >= objs.size() || objs[size_t(object)].walkGraph.empty()) return nullptr;
    Bytes text;
    if (!fs_.read(objs[size_t(object)].walkGraph, text) && !fs_.read("Objects/" + objs[size_t(object)].walkGraph, text)) {
        SDL_Log("walkable graph not found: %s", objs[size_t(object)].walkGraph.c_str());
        return nullptr;
    }
    std::string src(text.begin(), text.end()), geom;
    std::vector<std::pair<int, std::vector<int>>> verts;
    size_t pos = 0;
    while (pos < src.size()) {
        size_t eol = src.find('\n', pos);
        if (eol == std::string::npos) eol = src.size();
        std::string line = src.substr(pos, eol - pos);
        pos = eol + 1;
        size_t a = line.find_first_not_of(" \t\r");
        if (a == std::string::npos || line[a] == '#') continue;
        line = line.substr(a);
        if (line.compare(0, 14, "Geometry_File:") == 0) {
            size_t b = line.find_first_not_of(" \t\"", 14);
            if (b != std::string::npos) {
                size_t e = line.find_first_of(" \t\r\"}", b);
                geom = line.substr(b, e == std::string::npos ? std::string::npos : e - b);
            }
        } else if (line.compare(0, 6, "Vertex") == 0) {
            int n = -1;
            std::vector<int> l;
            const char* c = line.c_str() + 6;
            char* end = nullptr;
            n = int(std::strtol(c, &end, 10));
            const char* b = std::strchr(end, '{');
            for (c = b ? b + 1 : end; c && *c && *c != '}';) {
                long v = std::strtol(c, &end, 10);
                if (end == c) { c++; continue; }
                l.push_back(int(v)), c = end;
            }
            verts.push_back({n, l});
        }
    }
    // the geometry (.six names the model the sim ships as .M)
    std::string key = upper(geom);
    size_t dot = key.rfind('.');
    if (dot != std::string::npos) key = key.substr(0, dot) + ".M";
    Bytes mb;
    Model m;
    if (geom.empty() || !fs_.read(modelKey(fs_, key), mb) || !loadModel(mb, m) || m.frames.empty()) {
        SDL_Log("walkable graph %s: geometry %s not loaded", objs[size_t(object)].walkGraph.c_str(), geom.c_str());
        return nullptr;
    }
    Mat4 root = Mat4::identity();
    for (auto& i : instances_)
        if (i.object == object) root = i.world;
    auto ng = std::make_unique<GenGraph>();
    std::function<void(int, const Mat4&)> walk = [&](int fi, const Mat4& parent) {
        while (fi >= 0 && size_t(fi) < m.frames.size() && ng->nodes.size() < 0x800) {
            const Frame& f = m.frames[size_t(fi)];
            Mat4 w = fi == 0 ? root : frameMatrix(f) * parent;
            ng->nodes.push_back(Vec3(w.m[3][0], w.m[3][1], w.m[3][2]));
            if (f.child >= 0) walk(f.child, w);
            fi = f.sibling;
        }
    };
    walk(0, Mat4::identity());
    ng->links.assign(ng->nodes.size(), {});
    ng->taken.assign(ng->nodes.size(), false);
    for (auto& [n, l] : verts)  // FUN_10084f90 keeps a Vertex line only for a node that exists
        if (n >= 0 && size_t(n) < ng->nodes.size() && !l.empty()) ng->links[size_t(n)] = l;
    SDL_Log("walkable graph %s on object %d: %zu nodes, %zu linked", objs[size_t(object)].walkGraph.c_str(), object,
            ng->nodes.size(), verts.size());
    g = std::move(ng);
    return g.get();
}

bool Scene::graphNodeAt(const Vec3& p, int& object, int& node) {
    auto cheb = [](const Vec3& a, const Vec3& b) { return std::max({std::fabs(a.x - b.x), std::fabs(a.y - b.y), std::fabs(a.z - b.z)}); };
    const auto& objs = mission_.objects;
    for (size_t o = 0; o < objs.size(); o++) {
        if (objs[o].walkGraph.empty() || objectHidden(int(o))) continue;
        const GenGraph* g = walkGraph(int(o));
        Vec3 op;
        if (!g || g->nodes.empty() || !objectPosition(int(o), op) || cheb(op, p) > objectRadius(int(o))) continue;
        float best = 1e30f;
        int bi = -1;
        for (size_t n = 0; n < g->nodes.size(); n++) {
            float d = cheb(g->nodes[n], p);
            if (d < best) {
                best = d, bi = int(n);
                if (d <= 2.f) break;
            }
        }
        object = int(o), node = bi;
        return bi >= 0;
    }
    return false;
}

bool Scene::frameWorld(int object, const std::string& frame, Vec3& out) const {
    for (auto& i : instances_) {
        if (i.object != object || !i.rig) continue;
        bool found = false;
        walkPose(i, [&](int, const Frame& f, const Mat4& w, bool) {
            if (!found && frameIs(f.name, frame)) out = Vec3(w.m[3][0], w.m[3][1], w.m[3][2]), found = true;
        });
        return found;
    }
    return false;
}

namespace {
// A looping set's real cycle: some sets end with a padding key that repeats the previous one (the Jaguar's walk has
// its keys to 39 and again at 49), which held the pose for 10 frames each loop - a stop in every stride.
int cycleEnd(const AnimSet& set) {
    int end = 0;
    auto chan = [&](const std::vector<AnimKey>& ks) {
        if (ks.empty()) return;
        size_t j = ks.size() - 1;
        while (j > 0 && std::memcmp(ks[j - 1].v, ks.back().v, sizeof ks.back().v) == 0) --j;
        end = std::max(end, int(ks[j].time));
    };
    for (auto& t : set.tracks) chan(t.rot), chan(t.pos), chan(t.scale);
    return end > 0 ? end : set.length;
}
}  // namespace

Vec3 Scene::animTravel(int object, int animSet, float t) const {
    // what walkPose takes out of the top frames at time t (their horizontal travel since the set's first key), in the
    // model's space: the root's own, plus each child's turned by the root's posed rotation
    for (auto& i : instances_) {
        if (i.object != object || !i.rig || !i.sets || animSet < 0 || size_t(animSet) >= i.sets->size()) continue;
        const AnimSet& set = (*i.sets)[size_t(animSet)];
        const Model& m = i.rig->model;
        auto track = [&](int fi) -> const AnimTrack* {
            std::string n = lower(m.frames[size_t(fi)].name);
            for (auto& trk : set.tracks)
                if (trk.frame == n) return &trk;
            return nullptr;
        };
        Vec3 sum;
        for (int fi = 0; fi >= 0 && fi < int(m.frames.size()); fi = m.frames[size_t(fi)].sibling) {
            const AnimTrack* tr = track(fi);
            Mat4 local = animLocal(tr, frameMatrix(m.frames[size_t(fi)]), t);
            if (tr && !tr->pos.empty()) {
                Mat4 l0 = animLocal(tr, frameMatrix(m.frames[size_t(fi)]), 0.f);
                sum += Vec3(local.m[3][0] - l0.m[3][0], 0, local.m[3][2] - l0.m[3][2]);
            }
            for (int c = m.frames[size_t(fi)].child; c >= 0 && c < int(m.frames.size()); c = m.frames[size_t(c)].sibling) {
                const AnimTrack* ct = track(c);
                if (!ct || ct->pos.empty()) continue;
                Mat4 a = animLocal(ct, frameMatrix(m.frames[size_t(c)]), t), b = animLocal(ct, frameMatrix(m.frames[size_t(c)]), 0.f);
                sum += local.transformDir(Vec3(a.m[3][0] - b.m[3][0], 0, a.m[3][2] - b.m[3][2]));
            }
        }
        return sum;
    }
    return Vec3();
}

bool Scene::frameBottom(int object, const std::string& frame, Vec3& out) const {
    for (auto& i : instances_) {
        if (i.object != object || !i.rig) continue;
        bool found = false;
        walkPose(i, [&](int fi, const Frame& f, const Mat4& w, bool) {
            if (found || !frameIs(f.name, frame)) return;
            const GpuMesh* g = i.rig->parts[size_t(fi)];
            if (!g) {
                out = Vec3(w.m[3][0], w.m[3][1], w.m[3][2]);
            } else {
                // the sole: whichever of the box's two Y faces is lower as posed (the gears' foot parts have local +Y
                // pointing down, so bmin.y is the top of the foot)
                float cx = (g->bmin.x + g->bmax.x) * 0.5f, cz = (g->bmin.z + g->bmax.z) * 0.5f;
                Vec3 a = w.transformPoint(Vec3(cx, g->bmin.y, cz)), b = w.transformPoint(Vec3(cx, g->bmax.y, cz));
                out = a.y <= b.y ? a : b;
            }
            found = true;
        });
        return found;
    }
    return false;
}

std::string Scene::nearestFrame(int object, const Vec3& p, const std::vector<std::string>& frames) const {
    std::string best;
    float bd = 1e18f;
    for (auto& i : instances_) {
        if (i.object != object || !i.rig) continue;
        walkPose(i, [&](int fi, const Frame& f, const Mat4& w, bool hidden) {
            if (hidden) return;
            for (auto& want : frames)
                if (frameIs(f.name, want)) {
                    // measure to the middle of the part's mesh when it has one, else to the joint
                    Vec3 c(w.m[3][0], w.m[3][1], w.m[3][2]);
                    if (const GpuMesh* g = i.rig->parts[size_t(fi)]) c = w.transformPoint((g->bmin + g->bmax) * 0.5f);
                    float d = dot(c - p, c - p);
                    if (d < bd) bd = d, best = want;
                }
        });
        break;
    }
    return best;
}

bool Scene::frameUnder(int object, const std::string& frame, const std::string& ancestor) const {
    for (auto& i : instances_) {
        if (i.object != object || !i.rig) continue;
        const Model& m = i.rig->model;
        bool result = false;
        std::function<void(int, bool)> walk = [&](int fi, bool inside) {
            while (fi >= 0 && fi < int(m.frames.size())) {
                const Frame& f = m.frames[size_t(fi)];
                bool in = inside || frameIs(f.name, ancestor);
                if (in && frameIs(f.name, frame)) result = true;
                if (f.child >= 0) walk(f.child, in);
                fi = f.sibling;
            }
        };
        walk(0, false);
        return result;
    }
    return false;
}

void Scene::setFrameDamage(int object, const std::string& frame, const std::string& tex, float amount) {
    for (auto& i : instances_) {
        if (i.object != object) continue;
        if (i.damageTex < 0 && !tex.empty()) i.damageTex = texture(tex);
        std::string f = lower(frame);
        bool found = false;
        for (auto& d : i.damagedFrames)
            if (d.first == f) d.second = amount, found = true;
        if (!found) i.damagedFrames.push_back({f, amount});
    }
}

void Scene::hideFrame(int object, const std::string& frame) {
    for (auto& i : instances_)
        if (i.object == object) i.hiddenFrames.push_back(lower(frame)), i.poseDirty = true;
}

void Scene::buildTerrain() {
    std::map<int, std::vector<Vertex>> byTex;
    std::vector<int> texIds;
    for (auto& t : terrain_.textures) texIds.push_back(texture(t));
    for (const TerrainTile& t : terrain_.tiles) {
        int ti = t.tex[0];
        int tex = ti >= 0 && size_t(ti) < texIds.size() ? texIds[size_t(ti)] : r_.whiteTexture();
        int rot = t.dim == 3 ? (t.f[19] & 3) : 0;
        auto& dst = byTex[tex];
        float span = float(t.x1 - t.x0);
        auto vtx = [&](int ix, int iz) {
            Vertex v{};
            float x = float(t.x0 + ix * t.step), z = float(t.z0 + iz * t.step);
            v.pos[0] = x, v.pos[2] = z;
            v.pos[1] = t.h[size_t(ix) * t.dim + iz] * 0.01f;
            // T-junctions: a 5x5 tile next to a 3x3 one has an extra vertex mid-edge that the coarse tile doesn't; left
            // at its own height it opens a crack (the "tear" where the water / fog shows through). Edge vertices take
            // the coarser neighbour's height along the shared edge.
            if (ix == 0 || iz == 0 || ix == t.dim - 1 || iz == t.dim - 1) {
                float nx = ix == 0 ? -0.05f : ix == t.dim - 1 ? 0.05f : 0.f, nz = iz == 0 ? -0.05f : iz == t.dim - 1 ? 0.05f : 0.f;
                for (int k = 0; k < 2; k++) {
                    float px = x + (k == 0 ? nx : 0.f), pz = z + (k == 1 ? nz : 0.f);
                    if ((k == 0 && nx == 0.f) || (k == 1 && nz == 0.f) || px < 0 || pz < 0 || px >= 4096 || pz >= 4096) continue;
                    const TerrainTile& n = terrain_.tiles[size_t(int(px) / 32) * 128 + size_t(int(pz) / 32)];
                    if (n.dim < t.dim) v.pos[1] = terrain_.sample(px, pz);
                }
            }
            float e = 4.f;
            float hx = terrain_.sample(x + e, z) - terrain_.sample(x - e, z);
            float hz = terrain_.sample(x, z + e) - terrain_.sample(x, z - e);
            Vec3 n = normalize(Vec3(-hx, 2 * e, -hz));
            v.normal[0] = n.x, v.normal[1] = n.y, v.normal[2] = n.z;
            // texture axes follow the file's grid, which is transposed relative to the world (see loadTerrain)
            float u = (z - t.z0) / span, w = (x - t.x0) / span;
            for (int k = 0; k < rot; k++) {
                float nu = w, nw = 1.f - u;
                u = nu, w = nw;
            }
            v.uv[0] = u, v.uv[1] = w;
            v.color[0] = v.color[1] = v.color[2] = v.color[3] = 255;
            return v;
        };
        for (int ix = 0; ix + 1 < t.dim; ix++)
            for (int iz = 0; iz + 1 < t.dim; iz++) {
                Vertex a = vtx(ix, iz), b = vtx(ix + 1, iz), c = vtx(ix + 1, iz + 1), d = vtx(ix, iz + 1);
                // each cell's own diagonal (attribute bit 15), as Terrain::sample follows it
                if (Terrain::mainDiagonal(t, ix, iz)) dst.insert(dst.end(), {a, b, c, a, c, d});
                else dst.insert(dst.end(), {a, b, d, b, c, d});
            }
    }
    std::vector<Vertex> all;
    std::vector<DrawBatch> batches;
    for (auto& [tex, vs] : byTex) {
        batches.push_back({tex, uint32_t(all.size()), uint32_t(vs.size()), false, false, false, true});
        all.insert(all.end(), vs.begin(), vs.end());
    }
    terrainMesh_ = r_.createMesh(all, std::move(batches));
}

// FUN_1003f950 (on the bridge's first update) / FUN_1003f6a0: the ends are the frames HP1 and HP2, each put on the
// ground under it (FUN_100a4070) and then both at the lower height; the box is the object's (FUN_10019df0); the
// widths come from FUN_1003f370 (the smaller horizontal size w of the box: +0xe80 w, +0xe88 w / 2, +0xe84 w / 8).
void Scene::addBridge(int idx, const std::string& mdl) {
    const Instance& i = instances_[size_t(idx)];
    Bytes b;
    Model m;
    if (!fs_.read(modelKey(fs_, mdl), b) || !loadModel(b, m)) return;
    Bridge br;
    br.object = i.object;
    int found = 0;
    std::function<void(int, const Mat4&)> walk = [&](int fi, const Mat4& parent) {
        while (fi >= 0 && fi < int(m.frames.size())) {
            const Frame& f = m.frames[size_t(fi)];
            Mat4 w = frameMatrix(f) * parent;
            for (int k = 0; k < 2; k++)
                if (frameIs(f.name, k ? "hp2" : "hp1")) {
                    Mat4 ww = w * i.world;
                    br.end[k] = Vec3(ww.m[3][0], ww.m[3][1], ww.m[3][2]);
                    br.axis[k] = normalize(Vec3(ww.m[2][0], ww.m[2][1], ww.m[2][2]));
                    br.xaxis[k] = normalize(Vec3(ww.m[0][0], ww.m[0][1], ww.m[0][2]));
                    found |= 1 << k;
                }
            if (f.child >= 0) walk(f.child, w);
            fi = f.sibling;
        }
    };
    walk(0, Mat4::identity());
    if (found != 3) return;
    for (auto& e : br.end) e.y = groundHeight(e.x, e.z);
    br.end[0].y = br.end[1].y = std::min(br.end[0].y, br.end[1].y);
    instanceBox(i, br.lo, br.hi);
    float w = std::min(br.hi.x - br.lo.x, br.hi.z - br.lo.z);
    br.halfW = w * 0.5f, br.lineTol = w * 0.125f;
    bridgeOf_[size_t(idx)] = int(bridges_.size());
    bridges_.push_back(br);
    SDL_Log("bridge %d: ends %.0f,%.1f,%.0f and %.0f,%.1f,%.0f", br.object, br.end[0].x, br.end[0].y, br.end[0].z, br.end[1].x,
            br.end[1].y, br.end[1].z);
}

void Scene::addInstance(size_t index) {
        const MissionObject& o = mission_.objects[index];
        if (o.defIndex < 0 || size_t(o.defIndex) >= mission_.defs.size()) return;
        const MissionDef& def = mission_.defs[size_t(o.defIndex)];
        const std::string& mdl = def.firstModel;
        if (mdl.empty()) return;
        Instance inst;
        if (!def.animScript.empty()) {
            inst.rig = rigged(mdl);
            inst.sets = animSets(def.animScript);
        }
        if (!inst.rig) inst.mesh = model(mdl);
        if (!inst.mesh && !inst.rig) return;
        Mat4 rot = Mat4::rotZ(float(o.rot[2]) * kDeg) * Mat4::rotX(float(o.rot[1]) * kDeg) *
                   Mat4::rotY(float(o.rot[0]) * kDeg);
        Mat4 world = rot * Mat4::translate(Vec3(float(o.pos[0]), float(o.pos[1]), float(o.pos[2])));
        inst.world = world;
        inst.object = int(index);
        // Not drawn in game: the spawn points (0x56-0x59; FUN_10085570 / FUN_100883d0 build them as team 1-4 markers that
        // only join the world's spawn list at +0x2860, never the object list that is drawn) and objects carrying
        // Light_Information (0x43): the factory makes the light, gives it the object's name and placement and then
        // releases the object itself (lampicon.m is the editor's icon).
        if ((o.type >= 0x56 && o.type <= 0x59) || o.isLight) inst.noDraw = true;
        // In the collision world (FUN_100a5820) unless the class constructor cleared the object's [5] bit 0x40000:
        // Object_NoColl (0x162, also [4] & ~0x10000) and Plane_NoColl (0x61) (object factories FUN_10085570 /
        // FUN_100883d0). Tree_Object (0x47) instances are made by FUN_10079ef0 (vtable 0x10145c38) into the tree list
        // (DAT_101fcd78) that the mission loader (FUN_1008dba0) adds to the world's plain object list (+0x2858) only:
        // trees have no proxy, so nothing collides with them. The spawn point markers (0x56-0x59) never join the
        // object lists and lights release their object (see above); 0x5c is a marker too.
        bool noColl = o.type == 0x162 || o.type == 0x61 || o.type == 0x47 || (o.type >= 0x56 && o.type <= 0x59) ||
                      o.type == 0x5c || o.isLight;
        inst.collides = !noColl;
        // Unit classes (biped 0x48, Jeep 0x4e, hovertank 0x4f, infantry 0x50, mount 0x51, chicken walker 0x52, frame
        // 0x53, ghosts 0x13d - FUN_10066d10, a unit with a brain): dynamic proxies, tested against other units only in
        // pairs that hold the player (FUN_100a0b00), and never obstacles for the pathfinder (vehicle base [4] bit)
        inst.unit = (o.type >= 0x48 && o.type <= 0x53 && o.type != 0x49 && o.type != 0x4a && o.type != 0x4b &&
                     o.type != 0x4c && o.type != 0x4d) || o.type == 0x13d;
        if (inst.mesh) {
            std::string key = modelKey(fs_, mdl);
            auto ci = coll_.find(key);
            if (ci != coll_.end() && ci->second && !ci->second->empty() && inst.collides) inst.coll = ci->second.get();
            // rigid inverse: transpose the rotation, rotate the negated translation
            Mat4 inv = Mat4::identity();
            for (int r2 = 0; r2 < 3; r2++)
                for (int c = 0; c < 3; c++) inv.m[r2][c] = world.m[c][r2];
            Vec3 t(world.m[3][0], world.m[3][1], world.m[3][2]);
            Vec3 it = inv.transformDir(t);
            inv.m[3][0] = -it.x, inv.m[3][1] = -it.y, inv.m[3][2] = -it.z;
            inst.inv = inv;
        }
        instances_.push_back(inst);
        int idx = int(instances_.size()) - 1;
        if (bridgeOf_.size() < instances_.size()) bridgeOf_.resize(instances_.size(), -1);
        if (o.type == 0x13f && instances_.back().coll) addBridge(idx, mdl);
        if (!o.walkGraph.empty()) addWalkGraph(idx, o.walkGraph);
        if (instances_.back().coll) {
            instanceBox(instances_.back(), instances_.back().wlo, instances_.back().whi);
            gridInsert(idx);
        } else if (instances_.back().rig && instances_.back().collides) {
            movers_.push_back(idx);
        }
    }

int Scene::addObject(const MissionObject& o) {
    mission_.objects.push_back(o);
    size_t idx = mission_.objects.size() - 1;
    addInstance(idx);
    return int(idx);
}

bool Scene::load(const std::string& missionName, std::string* err, const std::function<void(Mission&)>& prepare) {
    std::string mn = missionName;
    if (extension(mn) != "ann") mn += ".ANN";
    Bytes b;
    if (!fs_.read("mission/" + mn, b) && !fs_.read(mn, b)) {
        if (err) *err = "mission not found: " + mn;
        return false;
    }
    if (!loadMission(b, mission_, err)) return false;
    if (prepare) prepare(mission_);
    SDL_Log("mission '%s': terrain %s, %zu objects, %zu definitions", mission_.title.c_str(),
            mission_.terrain.c_str(), mission_.objects.size(), mission_.defs.size());
    if (!mission_.terrain.empty()) {
        Bytes tb;
        if (fs_.read(mission_.terrain, tb) && loadTerrain(tb, terrain_)) {
            hasTerrain_ = true;
            buildTerrain();
        } else {
            SDL_Log("terrain failed: %s", mission_.terrain.c_str());
        }
    }
    if (!mission_.skyModel.empty()) sky_ = model(mission_.skyModel);
    if (mission_.hasWater) {
        int tex = texture(mission_.waterTexture);
        std::vector<Vertex> w;
        float y = mission_.waterLevel;
        auto v = [&](float x, float z) {
            Vertex o{};
            o.pos[0] = x, o.pos[1] = y, o.pos[2] = z;
            o.normal[1] = 1;
            o.uv[0] = x / 64.f, o.uv[1] = z / 64.f;
            o.color[0] = o.color[1] = o.color[2] = 255;
            o.color[3] = uint8_t(std::max(0, std::min(255, mission_.waterAlpha)));
            return o;
        };
        w = {v(-4096, -4096), v(8192, -4096), v(8192, 8192), v(-4096, -4096), v(8192, 8192), v(-4096, 8192)};
        waterMesh_ = r_.createMesh(w, {{tex, 0, 6, true, false}});
    }
    grid_.assign(128 * 128, {});
    for (size_t i = 0; i < mission_.objects.size(); i++) addInstance(i);
    return true;
}

FrameParams Scene::frameDefaults() const {
    FrameParams fp;
    const Mission& m = mission_;
    fp.fogColor = Vec3(m.fog[0] / 255.f, m.fog[1] / 255.f, m.fog[2] / 255.f);
    fp.clearColor = fp.fogColor;
    fp.fogStart = m.fogMin > 0 ? m.fogMin : 150.f;
    fp.fogEnd = m.fogMax > fp.fogStart ? m.fogMax : 300.f;
    fp.fogIntensity = m.fogIntensity > 0 ? m.fogIntensity : 1.f;
    Vec3 lc(m.lightColor[0] / 255.f, m.lightColor[1] / 255.f, m.lightColor[2] / 255.f);
    fp.lightColor = lc * m.lightDiffuse;
    fp.ambient = lc * m.lightAmbient;
    Vec3 ld(m.lightDir[0], m.lightDir[1], m.lightDir[2]);
    fp.lightDir = length(ld) > 0 ? ld : Vec3(-0.3f, 1, -0.2f);
    return fp;
}

void Scene::render(const FrameParams& fp) {
    if (sky_) {
        // Sky follows the camera, unlit, no fog depth (drawn first).
        r_.draw(sky_, Mat4::translate(fp.eye), false, 1.f, false, true);
    }
    if (terrainMesh_) r_.draw(terrainMesh_, Mat4::identity(), true);
    for (auto& i : instances_) {
        if (i.hidden || i.noDraw) continue;
        if (i.rig) {
            walkPose(i, [&](int fi, const Frame& f, const Mat4& w, bool hidden) {
                if (hidden || !i.rig->parts[size_t(fi)]) return;
                r_.draw(i.rig->parts[size_t(fi)], w, true);
                if (i.damageTex >= 0)
                    for (auto& d : i.damagedFrames)
                        if (d.second > 0.05f && frameIs(f.name, d.first))
                            r_.draw(i.rig->parts[size_t(fi)], w, true, std::min(1.f, d.second), true, false, i.damageTex);
            });
        } else {
            r_.draw(i.mesh, i.world, true);
        }
    }
    if (waterMesh_) r_.draw(waterMesh_, Mat4::translate(Vec3(0, waterLevel() - mission_.waterLevel, 0)), false);
}

void Scene::setObjectPose(int object, const Vec3& pos, float yaw, int animSet, float frame) {
    for (auto& i : instances_)
        if (i.object == object) {
            i.world = Mat4::rotY(yaw) * Mat4::translate(pos);
            i.inv = Mat4::translate(Vec3(-pos.x, -pos.y, -pos.z)) * Mat4::rotY(-yaw);
            i.set = animSet;
            i.frame = frame;
            i.poseDirty = true;
            if (i.coll) {  // a static model placed again (dropships, trains, elevators): its box follows it
                instanceBox(i, i.wlo, i.whi);
                if (!i.mover) i.mover = true, movers_.push_back(int(&i - instances_.data()));
            }
        }
}

void Scene::setObjectFade(int object, int animSet, float frame, float weight) {
    for (auto& i : instances_)
        if (i.object == object) i.fadeSet = weight > 0 ? animSet : -1, i.fadeFrame = frame, i.fadeWeight = weight, i.poseDirty = true;
}

void Scene::setObjectOverlay(int object, int animSet, float frame, const std::string& rootFrame) {
    for (auto& i : instances_)
        if (i.object == object) i.overlaySet = animSet, i.overlayFrame = frame, i.overlayRoot = lower(rootFrame), i.poseDirty = true;
}

void Scene::clearDamage(int object) {
    for (auto& i : instances_)
        if (i.object == object) i.hiddenFrames.clear(), i.damagedFrames.clear(), i.hidden = false, i.poseDirty = true;
}

namespace {
// world -> local for an affine row-vector matrix (animation keys may carry scale, so not just the transpose)
Mat4 affineInverse(const Mat4& m) {
    float a = m.m[0][0], b = m.m[0][1], c = m.m[0][2], d = m.m[1][0], e = m.m[1][1], f = m.m[1][2], g = m.m[2][0],
          h = m.m[2][1], k = m.m[2][2];
    float A = e * k - f * h, B = -(d * k - f * g), C = d * h - e * g;
    float det = a * A + b * B + c * C;
    Mat4 r = Mat4::identity();
    if (std::fabs(det) < 1e-12f) return r;
    float id = 1.f / det;
    r.m[0][0] = A * id, r.m[0][1] = -(b * k - c * h) * id, r.m[0][2] = (b * f - c * e) * id;
    r.m[1][0] = B * id, r.m[1][1] = (a * k - c * g) * id, r.m[1][2] = -(a * f - c * d) * id;
    r.m[2][0] = C * id, r.m[2][1] = -(a * h - b * g) * id, r.m[2][2] = (a * e - b * d) * id;
    Vec3 t(m.m[3][0], m.m[3][1], m.m[3][2]);
    Vec3 it = r.transformDir(t);
    r.m[3][0] = -it.x, r.m[3][1] = -it.y, r.m[3][2] = -it.z;
    return r;
}
int tileOf(float v) { return std::clamp(int(std::floor(v / 32.f)), 0, 127); }
}  // namespace

void Scene::posed(const Instance& i) const {
    if (!i.rig) return;
    size_t n = i.rig->model.frames.size();
    if (!i.poseDirty && i.pose.size() == n) return;
    i.pose.assign(n, Mat4::identity());
    i.poseHidden.assign(n, 0);
    walkPose(i, [&](int fi, const Frame&, const Mat4& w, bool hid) {
        if (fi >= 0 && size_t(fi) < n) i.pose[size_t(fi)] = w, i.poseHidden[size_t(fi)] = hid;
    });
    i.poseDirty = false;
}

// The proxy's box (FUN_100a5ff0 / FUN_100a6240): a static model's world box, at least 1 m on each axis; a rigged
// unit's bounding sphere.
void Scene::instanceBox(const Instance& i, Vec3& lo, Vec3& hi) const {
    lo = Vec3(1e9f, 1e9f, 1e9f), hi = Vec3(-1e9f, -1e9f, -1e9f);
    if (i.coll) {
        const Vec3 &a = i.coll->lo, &b = i.coll->hi;
        for (int k = 0; k < 8; k++) {
            Vec3 p = i.world.transformPoint(Vec3(k & 1 ? b.x : a.x, k & 2 ? b.y : a.y, k & 4 ? b.z : a.z));
            lo = Vec3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
            hi = Vec3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
        }
        // FUN_100a6240: an axis thinner than 1 m is widened by 0.5 m each way
        if (hi.x - lo.x < 1.f) lo.x -= 0.5f, hi.x += 0.5f;
        if (hi.y - lo.y < 1.f) lo.y -= 0.5f, hi.y += 0.5f;
        if (hi.z - lo.z < 1.f) lo.z -= 0.5f, hi.z += 0.5f;
    } else if (i.rig && i.rig->radius > 0) {
        Vec3 c = i.world.transformPoint(i.rig->centre);
        float r = i.rig->radius;
        lo = c - Vec3(r, r, r), hi = c + Vec3(r, r, r);
    }
}

void Scene::gridInsert(int idx) {
    const Instance& i = instances_[size_t(idx)];
    if (grid_.empty() || i.whi.x < i.wlo.x) return;
    for (int tz = tileOf(i.wlo.z); tz <= tileOf(i.whi.z); tz++)
        for (int tx = tileOf(i.wlo.x); tx <= tileOf(i.whi.x); tx++) grid_[size_t(tz * 128 + tx)].push_back(idx);
}

bool Scene::segmentInstance(const Instance& i, const Vec3& a, const Vec3& b, SegmentHit& hit, int object) const {
    bool found = false;
    if (i.coll) {
        CollHit ch;
        ch.t = hit.t;
        if (i.coll->segment(i.inv.transformPoint(a), i.inv.transformPoint(b), ch)) {
            hit.t = ch.t, hit.n = normalize(i.world.transformDir(ch.n)), hit.object = object;
            found = true;
        }
    } else if (i.rig) {
        posed(i);
        for (size_t fi = 0; fi < i.rig->coll.size() && fi < i.pose.size(); fi++) {
            const CollMesh* cm = i.rig->coll[fi].get();
            if (!cm || i.poseHidden[fi]) continue;
            Mat4 inv = affineInverse(i.pose[fi]);
            CollHit ch;
            ch.t = hit.t;
            if (cm->segment(inv.transformPoint(a), inv.transformPoint(b), ch)) {
                hit.t = ch.t, hit.n = normalize(i.pose[fi].transformDir(ch.n)), hit.object = object;
                found = true;
            }
        }
    }
    return found;
}

bool Scene::segment(const Vec3& a, const Vec3& b, int ignoreObject, int units, SegmentHit& hit, bool endInBox) const {
    static const bool prof = getenv("HG2_PROF") != nullptr;
    Uint64 t0 = prof ? SDL_GetTicksNS() : 0;
    struct Done {
        const Scene* s; Uint64 t0;
        ~Done() { if (t0) s->profNs += SDL_GetTicksNS() - t0, s->profCalls++; }
    } done{this, t0};
    Vec3 lo(std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z)), hi(std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z));
    if (seen_.size() < instances_.size()) seen_.resize(instances_.size(), 0);
    if (++seenStamp_ == 0) std::fill(seen_.begin(), seen_.end(), 0), seenStamp_ = 1;
    bool found = false;
    auto test = [&](int idx) {
        if (seen_[size_t(idx)] == seenStamp_) return;
        seen_[size_t(idx)] = seenStamp_;
        const Instance& i = instances_[size_t(idx)];
        if (i.hidden || !i.collides || i.object == ignoreObject || (!i.coll && !i.rig)) return;
        if (i.unit && (units == -1 || (units >= 0 && i.object != units))) return;
        Vec3 blo = i.wlo, bhi = i.whi;
        if (!i.coll) instanceBox(i, blo, bhi);
        if (hi.x < blo.x || lo.x > bhi.x || hi.y < blo.y || lo.y > bhi.y || hi.z < blo.z || lo.z > bhi.z) return;
        if (endInBox) {
            auto in = [&](const Vec3& p) {
                return p.x >= blo.x && p.x <= bhi.x && p.y >= blo.y && p.y <= bhi.y && p.z >= blo.z && p.z <= bhi.z;
            };
            if (!in(a) && !in(b)) return;
        }
        if (segmentInstance(i, a, b, hit, i.object)) found = true;
    };
    if (!grid_.empty())
        for (int tz = tileOf(lo.z); tz <= tileOf(hi.z); tz++)
            for (int tx = tileOf(lo.x); tx <= tileOf(hi.x); tx++)
                for (int idx : grid_[size_t(tz * 128 + tx)])
                    if (!instances_[size_t(idx)].mover) test(idx);
    for (int idx : movers_) test(idx);
    return found;
}

int Scene::raycast(const Vec3& a, const Vec3& b, int ignoreObject, float& tOut) const {
    SegmentHit h;
    segment(a, b, ignoreObject, -2, h);
    tOut = h.t;
    return h.object;
}

bool Scene::bridgeWalkable(const Bridge& b, const Vec3& p, int& end) const {
    end = -1;
    // FUN_1003faa0: inside the object's box
    bool inside = p.x >= b.lo.x && p.x <= b.hi.x && p.y >= b.lo.y && p.y <= b.hi.y && p.z >= b.lo.z && p.z <= b.hi.z;
    float proj[2] = {-1.f, -1.f};
    for (int k = 0; k < 2; k++) {  // FUN_10040430
        const Vec3& t = b.end[k];
        if (p.y < t.y - 1.f) continue;
        if (std::fabs(p.x - t.x) <= b.square && std::fabs(p.z - t.z) <= b.square) {  // FUN_10040360: the end's square
            end = k;
            return true;
        }
        proj[k] = dot(p - t, b.axis[k]);
        if (proj[k] >= 0) return inside || length(p - t) <= b.halfW;  // past the end, off the bridge
    }
    // between the ends, at deck height, near the line joining them (FUN_100b5c40)
    if (p.y >= b.end[0].y - 1.f && proj[0] < 0 && proj[1] < 0) {
        Vec3 d = b.end[1] - b.end[0];
        float dd = dot(d, d), u = dd > 0 ? std::clamp(dot(p - b.end[0], d) / dd, 0.f, 1.f) : 0.f;
        if (length(p - (b.end[0] + d * u)) < b.lineTol) return true;
    }
    return false;
}

int Scene::objectTest(const Vec3& p, float r, int self, int* by, int* end) const {
    if (end) *end = -1;
    if (grid_.empty()) return 0;
    auto test = [&](int idx) {
        const Instance& i = instances_[size_t(idx)];
        if (i.hidden || !i.coll || i.object == self || i.unit) return 0;
        if (size_t(idx) < bridgeOf_.size() && bridgeOf_[size_t(idx)] >= 0) {  // vtable +0xb4 / +0xb8
            int e;
            int bi = bridgeOf_[size_t(idx)];
            if (bridgeWalkable(bridges_[size_t(bi)], p, e)) {
                if (end && e >= 0) *end = bi * 2 + e;
                if (by) *by = i.object;
                return 2;
            }
        }
        // FUN_100c20e0 with the request's radius (+0xb8)
        if (i.wlo.z > p.z + r || i.whi.z < p.z - r || i.wlo.x > p.x + r || i.whi.x < p.x - r) return 0;
        if (i.wlo.y > p.y + 2.f * r || i.whi.y < p.y) return 0;
        if (by) *by = i.object;
        return 1;
    };
    int tx = tileOf(p.x), tz = tileOf(p.z);
    for (int idx : grid_[size_t(tz * 128 + tx)])
        if (!instances_[size_t(idx)].mover)
            if (int t = test(idx)) return t;
    for (int idx : movers_) {
        const Instance& i = instances_[size_t(idx)];
        if (i.coll && tileOf(i.wlo.x) <= tx && tx <= tileOf(i.whi.x) && tileOf(i.wlo.z) <= tz && tz <= tileOf(i.whi.z))
            if (int t = test(idx)) return t;
    }
    return 0;
}

float Scene::boundRadius(int object) const {
    for (auto& i : instances_) {
        if (i.object != object) continue;
        if (i.rig) return i.rig->radius;
        if (i.coll) return 0.5f * length(i.coll->hi - i.coll->lo);
        return 0.f;
    }
    return 0.f;
}

bool Scene::bodySegments(int object, Vec3& centre, std::vector<Vec3>& ends) const {
    ends.clear();
    for (auto& i : instances_) {
        if (i.object != object || !i.rig || i.rig->middle < 0) continue;
        posed(i);
        size_t mid = size_t(i.rig->middle);
        const CollMesh* cm = i.rig->coll[mid].get();
        centre = i.pose[mid].transformPoint((cm->lo + cm->hi) * 0.5f);  // the frame's sphere centre (+0xa0)
        for (size_t fi = 0; fi < i.rig->coll.size() && fi < i.pose.size(); fi++)
            if (i.rig->coll[fi] && !i.poseHidden[fi]) ends.push_back(Vec3(i.pose[fi].m[3][0], i.pose[fi].m[3][1], i.pose[fi].m[3][2]));
        return !ends.empty();
    }
    return false;
}

void Scene::hideObject(int object) {
    for (auto& i : instances_)
        if (i.object == object) i.hidden = true;
}
void Scene::setObjectTwist(int object, float yaw) {
    for (auto& i : instances_)
        if (i.object == object) i.twist = yaw, i.poseDirty = true;
}
void Scene::setObjectPitch(int object, float up) {
    for (auto& i : instances_)
        if (i.object == object) i.pitch = up, i.poseDirty = true;
}

void Scene::setObjectDrawn(int object, bool drawn) {
    for (auto& i : instances_)
        if (i.object == object) i.noDraw = !drawn;
}

void Scene::showObject(int object) {
    for (auto& i : instances_)
        if (i.object == object) i.hidden = false;
}

bool Scene::objectHidden(int object) const {
    for (auto& i : instances_)
        if (i.object == object) return i.hidden;
    return false;
}

float Scene::terrainRay(const Vec3& a, const Vec3& b) const {
    const int steps = 16;
    for (int k = 1; k <= steps; k++) {
        float t = float(k) / steps;
        Vec3 p = a + (b - a) * t;
        if (p.y < groundHeight(p.x, p.z)) return t;
    }
    return 2.f;
}


int Scene::animCycle(int object, int animSet) const {
    for (auto& i : instances_)
        if (i.object == object && i.sets && animSet >= 0 && size_t(animSet) < i.sets->size()) return cycleEnd((*i.sets)[size_t(animSet)]);
    return 0;
}

int Scene::animLength(int object, int animSet) const {
    for (auto& i : instances_)
        if (i.object == object && i.sets && size_t(animSet) < i.sets->size()) return (*i.sets)[size_t(animSet)].length;
    return 0;
}

const Surface* Scene::surfaceAt(float x, float z) const {
    if (!hasTerrain_ || mission_.surfaces.empty()) return nullptr;
    int idx = terrain_.surfaceAt(x, z);
    // FUN_10078570 clamps the index to the table and returns that entry
    int n = int(mission_.surfaces.size());
    return &mission_.surfaces[size_t(std::clamp(idx, 0, n - 1))];
}

float Scene::groundHeight(float x, float z) const {
    return hasTerrain_ ? terrain_.sample(x, z) : 0.f;
}

Vec3 Scene::groundNormal(float x, float z) const {
    return hasTerrain_ ? terrain_.normal(x, z) : Vec3(0, 1, 0);
}

bool Scene::objectBox(int object, Vec3& lo, Vec3& hi) const {
    for (auto& i : instances_) {
        if (i.object != object) continue;
        lo = Vec3(1e30f, 1e30f, 1e30f), hi = Vec3(-1e30f, -1e30f, -1e30f);
        auto grow = [&](const Vec3& a, const Vec3& b, const Mat4& w) {
            for (int k = 0; k < 8; k++) {
                Vec3 p = w.transformPoint(Vec3(k & 1 ? b.x : a.x, k & 2 ? b.y : a.y, k & 4 ? b.z : a.z));
                lo = Vec3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
                hi = Vec3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
            }
        };
        if (i.rig) {
            walkPose(i, [&](int fi, const Frame&, const Mat4& w, bool hidden) {
                if (const GpuMesh* g = i.rig->parts[size_t(fi)]; g && !hidden) grow(g->bmin, g->bmax, w);
            });
        } else if (i.mesh) {
            grow(i.mesh->bmin, i.mesh->bmax, i.world);
        }
        return hi.x >= lo.x;
    }
    return false;
}

float Scene::objectRadius(int object) const {
    Vec3 lo, hi;
    return objectBox(object, lo, hi) ? 0.5f * length(hi - lo) : 0.f;
}

bool Scene::objectCenter(int object, Vec3& out) const {
    Vec3 lo, hi;
    if (!objectBox(object, lo, hi)) return false;
    out = (lo + hi) * 0.5f;
    return true;
}

void Scene::setObjectPose(int object, const Vec3& pos, float yaw, int animSet, float frame, float pitch, float roll) {
    if (pitch == 0 && roll == 0) return setObjectPose(object, pos, yaw, animSet, frame);
    // rotX(+) lifts -Z (the nose); rotZ(+) tips +X up
    Mat4 w = Mat4::rotZ(roll) * Mat4::rotX(pitch) * Mat4::rotY(yaw) * Mat4::translate(pos);
    for (auto& i : instances_)
        if (i.object == object) {
            i.world = w;
            i.inv = affineInverse(w);
            i.set = animSet;
            i.frame = frame;
            i.poseDirty = true;
            if (i.coll) {
                instanceBox(i, i.wlo, i.whi);
                if (!i.mover) i.mover = true, movers_.push_back(int(&i - instances_.data()));
            }
        }
}

bool Scene::frameRest(int object, const std::string& frame, Vec3& origin, Vec3& lo, Vec3& hi) const {
    for (auto& i : instances_) {
        if (i.object != object) continue;
        Model own;  // a unit without an animation script is drawn as one mesh: read its frames from the file
        if (!i.rig) {
            const MissionObject& o = mission_.objects[size_t(object)];
            if (o.defIndex < 0 || size_t(o.defIndex) >= mission_.defs.size()) return false;
            Bytes b;
            if (!fs_.read(modelKey(fs_, mission_.defs[size_t(o.defIndex)].firstModel), b) || !loadModel(b, own)) return false;
        }
        const Model& m = i.rig ? i.rig->model : own;
        bool found = false;
        std::function<void(int, const Mat4&)> walk = [&](int fi, const Mat4& parent) {
            while (!found && fi >= 0 && fi < int(m.frames.size())) {
                const Frame& f = m.frames[size_t(fi)];
                Mat4 w = frameMatrix(f) * parent;
                if (frameIs(f.name, frame)) {
                    origin = Vec3(w.m[3][0], w.m[3][1], w.m[3][2]);
                    lo = hi = Vec3();
                    if (f.mesh >= 0 && size_t(f.mesh) < m.meshes.size() && !m.meshes[size_t(f.mesh)].v.empty()) {
                        lo = Vec3(1e30f, 1e30f, 1e30f), hi = Vec3(-1e30f, -1e30f, -1e30f);
                        for (const Vec3& v : m.meshes[size_t(f.mesh)].v)  // the frame's own mesh box (FUN_1001f3c0)
                            lo = Vec3(std::min(lo.x, v.x), std::min(lo.y, v.y), std::min(lo.z, v.z)),
                            hi = Vec3(std::max(hi.x, v.x), std::max(hi.y, v.y), std::max(hi.z, v.z));
                    }
                    found = true;
                    return;
                }
                if (f.child >= 0) walk(f.child, w);
                fi = f.sibling;
            }
        };
        walk(0, Mat4::identity());
        return found;
    }
    return false;
}

float Scene::boxVolume(int object) const {
    for (auto& i : instances_) {
        if (i.object != object) continue;
        Vec3 lo(1e30f, 1e30f, 1e30f), hi(-1e30f, -1e30f, -1e30f);
        if (i.coll) {
            lo = i.coll->lo, hi = i.coll->hi;
        } else if (i.rig) {
            const Model& m = i.rig->model;
            std::function<void(int, const Mat4&)> walk = [&](int fi, const Mat4& parent) {
                while (fi >= 0 && fi < int(m.frames.size())) {
                    const Frame& f = m.frames[size_t(fi)];
                    Mat4 w = frameMatrix(f) * parent;
                    if (size_t(fi) < i.rig->parts.size() && i.rig->parts[size_t(fi)]) {
                        const GpuMesh* g = i.rig->parts[size_t(fi)];
                        for (int k = 0; k < 8; k++) {
                            Vec3 p = w.transformPoint(Vec3(k & 1 ? g->bmax.x : g->bmin.x, k & 2 ? g->bmax.y : g->bmin.y,
                                                           k & 4 ? g->bmax.z : g->bmin.z));
                            lo = Vec3(std::min(lo.x, p.x), std::min(lo.y, p.y), std::min(lo.z, p.z));
                            hi = Vec3(std::max(hi.x, p.x), std::max(hi.y, p.y), std::max(hi.z, p.z));
                        }
                    }
                    if (f.child >= 0) walk(f.child, w);
                    fi = f.sibling;
                }
            };
            walk(0, Mat4::identity());
        }
        if (hi.x < lo.x) return 0.1f;
        return std::max(0.1f, (hi.x - lo.x) * (hi.y - lo.y) * (hi.z - lo.z));
    }
    return 0.1f;
}

bool Scene::isUnit(int object) const {
    for (auto& i : instances_)
        if (i.object == object) return i.unit;
    return false;
}

namespace {
// The graph geometry's frames (FUN_100954a0 loads the Geometry_File; FUN_1001a1a0 numbers its frames depth-first from
// the root - first child (+0x13c), then next sibling (+0x138) - and takes each frame's model-space origin). The .six
// files are the exporter's text dump (objects with parent / sibling / child words and a row-major local transform,
// object 0 the ROOT); the one .x file (tse20005.x) is DirectX text, whose top-level frames hang under an implicit root.
bool graphFrames(const std::string& text, bool dotX, std::vector<Vec3>& out) {
    struct F { Mat4 local = Mat4::identity(); int parent = -1; std::vector<int> kids; };
    std::vector<F> fr;
    auto lines = [&](auto&& fn) {
        size_t p = 0;
        while (p < text.size()) {
            size_t e = text.find('\n', p);
            if (e == std::string::npos) e = text.size();
            fn(text.substr(p, e - p));
            p = e + 1;
        }
    };
    if (!dotX) {
        std::vector<int> par, sib, kid;
        int row = -1;
        lines([&](std::string l) {
            auto num = [&]() {  // the first value on the line (hex word or float)
                size_t a = l.find_first_of("-0123456789");
                return a == std::string::npos ? std::string() : l.substr(a);
            };
            if (l.find(";name") != std::string::npos && l.find("string") != std::string::npos) {
                fr.push_back(F()), par.push_back(-1), sib.push_back(-1), kid.push_back(-1), row = -1;
            } else if (fr.empty()) {
                return;
            } else if (l.find(";parent") != std::string::npos) {
                par.back() = int(int32_t(std::stoul(num(), nullptr, 16)));
            } else if (l.find(";sibling") != std::string::npos) {
                sib.back() = int(int32_t(std::stoul(num(), nullptr, 16)));
            } else if (l.find(";child") != std::string::npos) {
                kid.back() = int(int32_t(std::stoul(num(), nullptr, 16)));
            } else if (l.find(";local transform") != std::string::npos) {
                row = 0;
            }
            if (row >= 0 && row < 4 && l.find("float") != std::string::npos) {
                float v[4] = {};
                std::string s = num();
                for (char& c : s) if (c == ',') c = ' ';
                if (sscanf(s.c_str(), "%f %f %f %f", &v[0], &v[1], &v[2], &v[3]) >= 3)
                    for (int c = 0; c < 3; c++) fr.back().local.m[row][c] = v[c];
                row++;
            }
        });
        for (size_t k = 0; k < fr.size(); k++) {
            fr[k].parent = par[k];
            for (int c = kid[k]; c >= 0 && size_t(c) < fr.size() && fr[k].kids.size() < fr.size(); c = sib[size_t(c)])
                fr[k].kids.push_back(c);
        }
    } else {
        fr.push_back(F());  // the implicit root
        std::vector<int> stack;  // per open brace: the frame it opens, or -1
        std::string tok;
        int pendingFrame = -1, want = 0;  // want: matrix numbers still to read
        float mat[16];
        int got = 0;
        bool inTemplate = false, templ = false;
        size_t i = 0;
        auto cur = [&]() { for (auto it = stack.rbegin(); it != stack.rend(); ++it) if (*it >= 0) return *it; return 0; };
        while (i < text.size()) {
            char c = text[i];
            if (c == '{') {
                if (templ || inTemplate) inTemplate = true, templ = false, stack.push_back(-2);
                else if (pendingFrame >= 0) stack.push_back(pendingFrame), pendingFrame = -1;
                else stack.push_back(-1);
                if (tok == "FrameTransformMatrix") want = 16, got = 0;
                tok.clear(), i++;
                continue;
            }
            if (c == '}') {
                if (!stack.empty()) {
                    if (stack.back() == -2) {
                        stack.pop_back();
                        inTemplate = false;
                        for (int s : stack) if (s == -2) inTemplate = true;
                    } else {
                        stack.pop_back();
                    }
                }
                tok.clear(), want = 0, i++;
                continue;
            }
            if (want > 0 && (std::isdigit(uint8_t(c)) || c == '-' || c == '.')) {
                size_t e = i;
                while (e < text.size() && (std::isdigit(uint8_t(text[e])) || text[e] == '-' || text[e] == '.' || text[e] == 'e' || text[e] == 'E' || text[e] == '+')) e++;
                mat[got++] = std::strtof(text.substr(i, e - i).c_str(), nullptr);
                if (--want == 0) {
                    Mat4 m = Mat4::identity();
                    for (int r = 0; r < 4; r++)
                        for (int k = 0; k < 3; k++) m.m[r][k] = mat[r * 4 + k];
                    fr[size_t(cur())].local = m;
                }
                i = e;
                continue;
            }
            if (std::isspace(uint8_t(c)) || c == ',' || c == ';') {
                i++;
                continue;
            }
            size_t e = i;
            while (e < text.size() && !std::isspace(uint8_t(text[e])) && text[e] != '{' && text[e] != '}') e++;
            std::string w = text.substr(i, e - i);
            if (w == "template") templ = true;
            if (!inTemplate && !templ && tok == "Frame") {  // "Frame <name> {"
                int parent = cur();
                fr.push_back(F());
                fr.back().parent = parent;
                fr[size_t(parent)].kids.push_back(int(fr.size()) - 1);
                pendingFrame = int(fr.size()) - 1;
            }
            tok = w, i = e;
        }
    }
    if (fr.empty()) return false;
    // depth first from the root: the frame, then its children in order
    out.clear();
    std::vector<int> seen(fr.size(), 0);
    std::function<void(int, const Mat4&)> visit = [&](int k, const Mat4& parent) {
        if (k < 0 || size_t(k) >= fr.size() || seen[size_t(k)]) return;
        seen[size_t(k)] = 1;
        Mat4 w = fr[size_t(k)].local * parent;
        out.push_back(Vec3(w.m[3][0], w.m[3][1], w.m[3][2]));
        for (int c : fr[size_t(k)].kids) visit(c, w);
    };
    visit(0, Mat4::identity());
    return !out.empty();
}
}  // namespace

// FUN_10084f90: the .grx text - "Geometry_File: <name>" (keyword 0x6e) loads the geometry and makes one node per
// frame; "Vertex <n> { a b ... }" (0xcd) gives node n its links (indices past the node count are ignored). After the
// object is placed FUN_1001a550 moves the nodes by its world matrix.
void Scene::addWalkGraph(int idx, const std::string& file) {
    Bytes b;
    if (!fs_.read("Objects/" + file, b) && !fs_.read(file, b)) {
        SDL_Log("walkable graph %s: not found", file.c_str());
        return;
    }
    std::string text(b.begin(), b.end());
    for (char& c : text) if (c == '\r') c = '\n';
    WalkGraph g;
    g.object = instances_[size_t(idx)].object;
    std::vector<std::pair<int, std::vector<int>>> verts;
    std::string geom;
    size_t p = 0;
    while (p < text.size()) {
        size_t e = text.find('\n', p);
        if (e == std::string::npos) e = text.size();
        std::string l = text.substr(p, e - p);
        p = e + 1;
        size_t h = l.find('#');
        if (h != std::string::npos) l.erase(h);
        if (size_t k = l.find("Geometry_File:"); k != std::string::npos) {
            std::string s = l.substr(k + 14);
            s.erase(0, s.find_first_not_of(" \t\""));
            s.erase(s.find_last_not_of(" \t\"") + 1);
            geom = s;
        } else if (size_t k2 = l.find("Vertex"); k2 != std::string::npos) {
            std::string s = l.substr(k2 + 6);
            while (s.find('}') == std::string::npos && p < text.size()) {  // a list may run over lines
                size_t e2 = text.find('\n', p);
                if (e2 == std::string::npos) e2 = text.size();
                s += " " + text.substr(p, e2 - p);
                p = e2 + 1;
            }
            for (char& c : s) if (c == '{' || c == '}') c = ' ';
            std::vector<int> v;
            const char* q = s.c_str();
            char* end;
            long n = std::strtol(q, &end, 10);
            if (end == q) continue;
            q = end;
            for (long a = std::strtol(q, &end, 10); end != q; a = std::strtol(q, &end, 10)) v.push_back(int(a)), q = end;
            verts.push_back({int(n), v});
        }
    }
    Bytes gb;
    if (geom.empty() || (!fs_.read("Objects/" + geom, gb) && !fs_.read(geom, gb))) {
        SDL_Log("walkable graph %s: geometry %s not found", file.c_str(), geom.c_str());
        return;
    }
    std::vector<Vec3> local;
    if (!graphFrames(std::string(gb.begin(), gb.end()), extension(geom) == "x", local)) return;
    const Mat4& w = instances_[size_t(idx)].world;
    for (auto& v : local) g.nodes.push_back(w.transformPoint(v));
    g.links.assign(g.nodes.size(), {});
    for (auto& [n, v] : verts)
        if (n >= 0 && size_t(n) < g.nodes.size())
            for (int a : v)
                if (a >= 0 && size_t(a) < g.nodes.size()) g.links[size_t(n)].push_back(a);
    // FUN_100c1da0 measures from the object's origin against its bounding radius (+0x98)
    g.centre = Vec3(w.m[3][0], w.m[3][1], w.m[3][2]);
    g.radius = boundRadius(g.object);
    SDL_Log("walkable graph %s on object %d: %zu nodes from %s, %zu linked", file.c_str(), g.object, g.nodes.size(),
            geom.c_str(), verts.size());
    graphs_.push_back(std::move(g));
}

bool Scene::nearestGraphNode(const Vec3& p, int& graph, int& node) const {
    // FUN_100c1da0 over the static objects listed on p's terrain tile; distances are FUN_100b5ce0's largest axis
    // difference. A graph object counts when p is within its bounding radius (+0x98) of its origin; its nearest node
    // is taken (the first within 2 m, DAT_10148418, at once) and the search stops there.
    auto cheb = [](const Vec3& a, const Vec3& b) {
        return std::max(std::fabs(a.x - b.x), std::max(std::fabs(a.y - b.y), std::fabs(a.z - b.z)));
    };
    graph = node = -1;
    for (size_t gi = 0; gi < graphs_.size(); gi++) {
        const WalkGraph& g = graphs_[gi];
        if (cheb(p, g.centre) > g.radius) continue;
        float best = 1e30f;
        for (size_t k = 0; k < g.nodes.size(); k++) {
            float d = cheb(g.nodes[k], p);
            if (d < best) {
                best = d, graph = int(gi), node = int(k);
                if (d <= 2.f) return true;
            }
        }
        if (graph >= 0) return true;
    }
    return false;
}

Vec3 Scene::startPosition() const {
    for (auto& o : mission_.objects)
        if (o.userHeader) return Vec3(float(o.pos[0]), float(o.pos[1]), float(o.pos[2]));
    return Vec3(2048, 50, 2048);
}

}  // namespace hg2
