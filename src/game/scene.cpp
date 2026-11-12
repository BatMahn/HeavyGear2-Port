#include "scene.h"

#include <algorithm>
#include <SDL3/SDL.h>

#include <cmath>
#include <functional>
#include <map>

namespace hg2 {

namespace {
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
    std::string key = upper(name);
    while (!key.empty() && key[0] == '@') key.erase(0, 1);  // '@' marks a shared/instanced geometry reference
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
        // collision boxes: bounds of each frame's mesh in model space
        auto& bx = boxes_[key];
        std::function<void(int, const Mat4&)> walk = [&](int fi, const Mat4& parent) {
            while (fi >= 0 && fi < int(m.frames.size())) {
                const Frame& f = m.frames[size_t(fi)];
                Mat4 w = frameMatrix(f) * parent;
                if (f.mesh >= 0 && !m.meshes[size_t(f.mesh)].v.empty()) {
                    Box b{Vec3(1e9f, 1e9f, 1e9f), Vec3(-1e9f, -1e9f, -1e9f)};
                    for (auto& p : m.meshes[size_t(f.mesh)].v) {
                        Vec3 q = w.transformPoint(p);
                        b.lo = Vec3(std::min(b.lo.x, q.x), std::min(b.lo.y, q.y), std::min(b.lo.z, q.z));
                        b.hi = Vec3(std::max(b.hi.x, q.x), std::max(b.hi.y, q.y), std::max(b.hi.z, q.z));
                    }
                    // skip decals / flat pieces the gears walk over
                    if (b.hi.y - b.lo.y > 0.8f && (b.hi.x - b.lo.x) * (b.hi.z - b.lo.z) < 250000.f) bx.push_back(b);
                }
                if (f.child >= 0) walk(f.child, w);
                fi = f.sibling;
            }
        };
        walk(0, Mat4::identity());
    } else {
        SDL_Log("model not found: %s", name.c_str());
    }
    models_[key] = mesh;
    return mesh;
}

Scene::Rigged* Scene::rigged(const std::string& name) {
    std::string key = upper(name);
    while (!key.empty() && key[0] == '@') key.erase(0, 1);
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
    const AnimSet* over = i.overlaySet >= 0 && i.sets && size_t(i.overlaySet) < i.sets->size() ? &(*i.sets)[size_t(i.overlaySet)] : nullptr;
    std::function<void(int, const Mat4&, bool, bool)> walk = [&](int fi, const Mat4& parent, bool hiddenBranch, bool inOverlay) {
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
            if (i.twist != 0 && frameIs(f.name, "tst")) local = Mat4::rotY(i.twist) * local;
            Mat4 w = local * parent;
            bool hid = hiddenBranch;
            for (auto& h : i.hiddenFrames) hid = hid || frameIs(f.name, h);
            fn(fi, f, w, hid);
            if (f.child >= 0) walk(f.child, w, hid, ov);
            fi = f.sibling;
        }
    };
    walk(0, i.world, false, false);
}

bool Scene::objectPosition(int object, Vec3& out) const {
    for (auto& i : instances_)
        if (i.object == object) {
            out = Vec3(i.world.m[3][0], i.world.m[3][1], i.world.m[3][2]);
            return true;
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
        if (i.object == object) i.hiddenFrames.push_back(lower(frame));
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
                dst.insert(dst.end(), {a, b, c, a, c, d});
            }
    }
    std::vector<Vertex> all;
    std::vector<DrawBatch> batches;
    for (auto& [tex, vs] : byTex) {
        batches.push_back({tex, uint32_t(all.size()), uint32_t(vs.size()), false, false});
        all.insert(all.end(), vs.begin(), vs.end());
    }
    terrainMesh_ = r_.createMesh(all, std::move(batches));
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
        if (inst.mesh) {
            std::string key = upper(mdl);
            while (!key.empty() && key[0] == '@') key.erase(0, 1);
            auto bi = boxes_.find(key);
            if (bi != boxes_.end() && !bi->second.empty()) inst.boxes = &bi->second;
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
    if (waterMesh_) r_.draw(waterMesh_, Mat4::translate(Vec3(0, waterOffset_, 0)), false);
}

void Scene::setObjectPose(int object, const Vec3& pos, float yaw, int animSet, float frame) {
    for (auto& i : instances_)
        if (i.object == object) {
            i.world = Mat4::rotY(yaw) * Mat4::translate(pos);
            i.inv = Mat4::translate(Vec3(-pos.x, -pos.y, -pos.z)) * Mat4::rotY(-yaw);
            i.set = animSet;
            i.frame = frame;
        }
}

void Scene::setObjectOverlay(int object, int animSet, float frame, const std::string& rootFrame) {
    for (auto& i : instances_)
        if (i.object == object) i.overlaySet = animSet, i.overlayFrame = frame, i.overlayRoot = lower(rootFrame);
}

void Scene::clearDamage(int object) {
    for (auto& i : instances_)
        if (i.object == object) i.hiddenFrames.clear(), i.damagedFrames.clear(), i.hidden = false;
}

bool Scene::collide(Vec3& pos, float radius, float height, int ignoreObject) const {
    bool hit = false;
    for (auto& i : instances_) {
        if (!i.boxes || i.object == ignoreObject || i.hidden) continue;
        Vec3 lp = i.inv.transformPoint(pos);
        for (auto& b : *i.boxes) {
            if (lp.x < b.lo.x - radius || lp.x > b.hi.x + radius || lp.z < b.lo.z - radius || lp.z > b.hi.z + radius) continue;
            if (b.hi.y < lp.y + 1.2f || b.lo.y > lp.y + height) continue;  // step over low parts, pass under high ones
            float cx = std::max(b.lo.x, std::min(lp.x, b.hi.x)), cz = std::max(b.lo.z, std::min(lp.z, b.hi.z));
            float dx = lp.x - cx, dz = lp.z - cz, d2 = dx * dx + dz * dz;
            if (d2 >= radius * radius) continue;
            if (d2 > 1e-6f) {
                float d = std::sqrt(d2), push = radius - d;
                lp.x += dx / d * push, lp.z += dz / d * push;
            } else {  // centre inside the box: leave by the nearest side
                float e[4] = {lp.x - b.lo.x, b.hi.x - lp.x, lp.z - b.lo.z, b.hi.z - lp.z};
                int k = int(std::min_element(e, e + 4) - e);
                if (k == 0) lp.x = b.lo.x - radius;
                else if (k == 1) lp.x = b.hi.x + radius;
                else if (k == 2) lp.z = b.lo.z - radius;
                else lp.z = b.hi.z + radius;
            }
            hit = true;
        }
        if (hit) pos = i.world.transformPoint(lp);
    }
    return hit;
}

int Scene::raycast(const Vec3& a, const Vec3& b, int ignoreObject, float& tOut) const {
    int best = -1;
    tOut = 2.f;
    for (auto& i : instances_) {
        if (i.object == ignoreObject || i.hidden) continue;
        if (!i.boxes) {
            if (!i.rig) continue;
            // animated units: test a 2.5 m x 7 m cylinder-ish box around the origin
            Vec3 o(i.world.m[3][0], i.world.m[3][1], i.world.m[3][2]);
            Vec3 d = b - a;
            float dd = dot(d, d);
            if (dd <= 0) continue;
            Vec3 c = o + Vec3(0, 3.5f, 0);
            float t = std::max(0.f, std::min(1.f, dot(c - a, d) / dd));
            Vec3 p = a + d * t;
            if (std::fabs(p.y - c.y) < 3.5f && (p.x - c.x) * (p.x - c.x) + (p.z - c.z) * (p.z - c.z) < 2.5f * 2.5f && t < tOut)
                tOut = t, best = i.object;
            continue;
        }
        Vec3 la = i.inv.transformPoint(a), lb = i.inv.transformPoint(b), d = lb - la;
        for (auto& bx : *i.boxes) {  // slab test
            float t0 = 0, t1 = 1;
            const float lo[3] = {bx.lo.x, bx.lo.y, bx.lo.z}, hi[3] = {bx.hi.x, bx.hi.y, bx.hi.z};
            const float o[3] = {la.x, la.y, la.z}, dv[3] = {d.x, d.y, d.z};
            bool miss = false;
            for (int k = 0; k < 3 && !miss; k++) {
                if (std::fabs(dv[k]) < 1e-6f) {
                    if (o[k] < lo[k] || o[k] > hi[k]) miss = true;
                    continue;
                }
                float u0 = (lo[k] - o[k]) / dv[k], u1 = (hi[k] - o[k]) / dv[k];
                if (u0 > u1) std::swap(u0, u1);
                t0 = std::max(t0, u0), t1 = std::min(t1, u1);
                if (t0 > t1) miss = true;
            }
            if (!miss && t0 < tOut) tOut = t0, best = i.object;
        }
    }
    return best;
}

void Scene::hideObject(int object) {
    for (auto& i : instances_)
        if (i.object == object) i.hidden = true;
}
void Scene::setObjectTwist(int object, float yaw) {
    for (auto& i : instances_)
        if (i.object == object) i.twist = yaw;
}

void Scene::setObjectDrawn(int object, bool drawn) {
    for (auto& i : instances_)
        if (i.object == object) i.noDraw = !drawn;
}

float Scene::supportHeight(const Vec3& pos, float step, int ignoreObject) const {
    float best = groundHeight(pos.x, pos.z);
    for (auto& i : instances_) {
        if (!i.boxes || i.object == ignoreObject || i.hidden) continue;
        Vec3 lp = i.inv.transformPoint(pos);
        for (auto& b : *i.boxes) {
            if (lp.x < b.lo.x || lp.x > b.hi.x || lp.z < b.lo.z || lp.z > b.hi.z) continue;
            float top = i.world.transformPoint(Vec3(lp.x, b.hi.y, lp.z)).y;
            if (top <= pos.y + step && top > best) best = top;
        }
    }
    return best;
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

Vec3 Scene::startPosition() const {
    for (auto& o : mission_.objects)
        if (o.userHeader) return Vec3(float(o.pos[0]), float(o.pos[1]), float(o.pos[2]));
    return Vec3(2048, 50, 2048);
}

}  // namespace hg2
