// hg2extract — inspect / extract Heavy Gear II data.
//   hg2extract <datadir> list                  list archive entries
//   hg2extract <datadir> get <name> <out>      extract one file (archives, loose, or ~-compressed)
//   hg2extract <datadir> check                 parse every model / terrain / mission and report
//   hg2extract <datadir> collaudit [MISSION|all] [-v]   static collision volumes, old vs new, suspicious ones listed
//   hg2extract <datadir> collboxes <model.m>   a model's collision boxes; meshes / materials <model.m> its parts
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <map>
#include <memory>
#include <string>

#include "../src/assets/assets.h"
#include "../src/assets/collision.h"
#include "../src/assets/mission.h"
#include "../src/assets/gear.h"
#include "../src/core/fs.h"
#include "../src/script/system.h"

using namespace hg2;

int main(int argc, char** argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: hg2extract <datadir> list|get <name> <out>|check\n");
        return 1;
    }
    FileSystem fs;
    if (!fs.mount(argv[1])) {
        fprintf(stderr, "no simpack.prj/simpack.map in %s\n", argv[1]);
        return 1;
    }
    std::string cmd = argv[2];
    if (cmd == "list") {
        for (auto& n : fs.sim().names()) printf("simpack  %s\n", n.c_str());
        for (auto& n : fs.shell().names()) printf("hg2shell %s\n", n.c_str());
        return 0;
    }
    if (cmd == "get" && argc >= 5) {
        Bytes b;
        if (!fs.read(argv[3], b)) {
            fprintf(stderr, "not found: %s\n", argv[3]);
            return 1;
        }
        FILE* f = fopen(argv[4], "wb");
        if (!f) return 1;
        fwrite(b.data(), 1, b.size(), f);
        fclose(f);
        return 0;
    }
    if (cmd == "check") {
        int ok = 0, bad = 0;
        for (auto& n : fs.sim().names()) {
            Bytes b;
            fs.read(n, b);
            if (extension(n) == "m") {
                Model m;
                if (loadModel(b, m)) ok++;
                else bad++, printf("model FAIL %s\n", n.c_str());
            } else if (extension(n) == "bmp" || extension(n) == "tga") {
                Image img;
                if (decodeImage(b, n, img)) ok++;
                else bad++, printf("image FAIL %s\n", n.c_str());
            }
        }
        for (const char* pre : {"D", "H", "M", "S", "T"})
            for (int i = 0; i < 40; i++) {
                char nm[32];
                snprintf(nm, sizeof nm, "mission/%s%02d.ann", pre, i);
                Bytes b;
                if (!fs.read(nm, b)) continue;
                Mission m;
                std::string err;
                if (!loadMission(b, m, &err)) {
                    bad++, printf("mission FAIL %s: %s\n", nm, err.c_str());
                    continue;
                }
                ok++;
                if (!m.terrain.empty()) {
                    Bytes tb;
                    Terrain t;
                    if (fs.read(m.terrain, tb) && loadTerrain(tb, t)) ok++;
                    else bad++, printf("terrain FAIL %s\n", m.terrain.c_str());
                }
            }
        printf("ok %d, failed %d\n", ok, bad);
        return bad ? 2 : 0;
    }
    if (cmd == "gears" && argc >= 4) {  // list a mission's gear / vehicle definitions
        Bytes b;
        Mission m;
        std::string err;
        if (!fs.read("mission/" + std::string(argv[3]) + ".ann", b) || !loadMission(b, m, &err)) return 1;
        for (size_t i = 0; i < m.defs.size(); i++) {
            GearDef g;
            if (!parseGearDef(m.defs[i].data, m.defs[i].size, g)) continue;
            printf("def %zu %s hp %d armor %d fc %d size %d, %zu damage parts, dmg tex %s\n", i, g.geometry[0].c_str(), g.hitPoints, g.armor, g.fireControl, g.size,
                   g.damageParts.size(), g.damageTexture.c_str());
            for (auto& d : g.damageParts) printf("   %-12s system %d armour %d\n", d.frame.c_str(), d.system, d.armour);
        }
        return 0;
    }
    if (cmd == "fountains" && argc >= 4) {
        Bytes b;
        Mission m;
        std::string err;
        if (!fs.read("mission/" + std::string(argv[3]) + ".ann", b) || !loadMission(b, m, &err)) return 1;
        for (size_t i = 0; i < m.fountainTemplates.size(); i++)
            printf("template %zu def %d type %x %s\n", i, m.fountainTemplates[i].defIndex, m.fountainTemplates[i].type,
                   m.fountainTemplates[i].script.c_str());
        for (auto& f : m.fountains) {
            printf("fountain '%s' team %d %s mode %d total %d alive %d wave %d flags %x at %.0f,%.0f,%.0f range %.0f/%.0f/%.0f script %s tpl",
                   f.name.c_str(), f.team, f.active ? "on" : "off", f.mode, f.total, f.maxAlive, f.wave, f.flags, f.pos[0], f.pos[1],
                   f.pos[2], f.range[0], f.range[1], f.range[2], f.script.c_str());
            for (int t : f.templates) printf(" %d", t);
            printf("\n");
        }
        printf("%zu objects, %zu paths, music track %d\n", m.objects.size(), m.paths.size(), m.musicTrack);
        return 0;
    }
    if (cmd == "objects" && argc >= 4) {  // list a mission's units (gears, vehicles; all objects with a 5th arg)
        Bytes b;
        Mission m;
        std::string err;
        if (!fs.read("mission/" + std::string(argv[3]) + ".ann", b) || !loadMission(b, m, &err)) return 1;
        for (size_t i = 0; i < m.objects.size(); i++) {
            const auto& o = m.objects[i];
            bool unit = o.type == 0x48 || o.type == 0x4b || o.type == 0x4e || o.type == 0x4f || o.type == 0x50 || o.type == 0x51 || o.type == 0x52;
            if (!unit && argc < 5 && !o.userHeader) continue;
            printf("%3zu %s type %x def %d (%s) team %d at %.0f,%.0f,%.0f script %s\n", i, o.userHeader ? "USER" : "    ", o.type, o.defIndex,
                   o.defIndex >= 0 ? m.defs[size_t(o.defIndex)].firstModel.c_str() : "-", o.team, o.pos[0], o.pos[1], o.pos[2], o.script.c_str());
        }
        return 0;
    }
    if (cmd == "meshes" && argc >= 4) {  // a model's frame meshes: name, model-space bounds, triangle area
        Bytes b;
        Model mdl;
        if (!fs.read(argv[3], b) || !loadModel(b, mdl)) return 1;
        std::function<void(int, const Mat4&)> walk = [&](int fi, const Mat4& parent) {
            while (fi >= 0 && fi < int(mdl.frames.size())) {
                const Frame& f = mdl.frames[size_t(fi)];
                Mat4 w = Mat4::identity();
                for (int r = 0; r < 4; r++)
                    for (int c = 0; c < 3; c++) w.m[r][c] = float(f.m[r * 3 + c]);
                w = w * parent;
                if (f.mesh >= 0) {
                    const Mesh& me = mdl.meshes[size_t(f.mesh)];
                    Vec3 lo(1e9f, 1e9f, 1e9f), hi(-1e9f, -1e9f, -1e9f);
                    std::vector<Vec3> wv;
                    for (auto& v : me.v) {
                        Vec3 q = w.transformPoint(v);
                        wv.push_back(q);
                        lo = Vec3(std::min(lo.x, q.x), std::min(lo.y, q.y), std::min(lo.z, q.z)), hi = Vec3(std::max(hi.x, q.x), std::max(hi.y, q.y), std::max(hi.z, q.z));
                    }
                    double area = 0;
                    for (auto& p : me.polys)
                        for (size_t k = 1; k + 1 < p.corners.size(); k++)
                            if (p.corners[k + 1].v < wv.size())
                                area += 0.5 * length(cross(wv[p.corners[k].v] - wv[p.corners[0].v], wv[p.corners[k + 1].v] - wv[p.corners[0].v]));
                    printf("%-24s %4zu polys (%.0f %.0f %.0f)-(%.0f %.0f %.0f) area %.0f\n", f.name.c_str(), me.polys.size(), lo.x, lo.y, lo.z, hi.x, hi.y, hi.z, area);
                }
                if (f.child >= 0) walk(f.child, w);
                fi = f.sibling;
            }
        };
        walk(0, Mat4::identity());
        return 0;
    }
    if (cmd == "materials" && argc >= 4) {  // a model's materials and their textures (alpha channel?)
        Bytes b;
        Model mdl;
        if (!fs.read(argv[3], b) || !loadModel(b, mdl)) return 1;
        for (size_t i = 0; i < mdl.materials.size(); i++) printf("%zu %s tex %s flags %x\n", i, mdl.materials[i].name.c_str(), mdl.materials[i].texture.c_str(), mdl.materials[i].flags);
        return 0;
    }
    if (cmd == "collboxes" && argc >= 4) {  // a model's collision boxes, old and new
        Bytes b;
        Model mdl;
        if (!fs.read(argv[3], b) || !loadModel(b, mdl)) return 1;
        TextureAlpha alpha(fs);
        CollModel o, n;
        legacyCollision(mdl, o);
        buildCollision(mdl, alpha.forModel(mdl), n);
        printf("mesh (%.1f %.1f %.1f)-(%.1f %.1f %.1f) cell %.2f samples %d transparent %d\n", n.meshLo.x, n.meshLo.y, n.meshLo.z, n.meshHi.x,
               n.meshHi.y, n.meshHi.z, n.cell, n.samples, n.transparentSamples);
        for (auto& x : o.full.boxes) printf("old (%.1f %.1f %.1f)-(%.1f %.1f %.1f)\n", x.lo.x, x.lo.y, x.lo.z, x.hi.x, x.hi.y, x.hi.z);
        for (auto& x : n.trunk.boxes) printf("trunk (%.1f %.1f %.1f)-(%.1f %.1f %.1f)\n", x.lo.x, x.lo.y, x.lo.z, x.hi.x, x.hi.y, x.hi.z);
        for (auto& x : n.full.boxes) printf("new (%.1f %.1f %.1f)-(%.1f %.1f %.1f)\n", x.lo.x, x.lo.y, x.lo.z, x.hi.x, x.hi.y, x.hi.z);
        return 0;
    }
    if (cmd == "collaudit") {  // collision audit: hgextract <data> collaudit [MISSION|all] [-v]
        // For every placed static object: the old one-box-per-mesh volumes against the column volumes, flagging boxes
        // much bigger than the solid geometry near the ground, markers that are never drawn, alpha cut-outs, wires.
        std::string want = argc >= 4 ? upper(argv[3]) : "ALL";
        bool verbose = argc >= 5;
        std::vector<std::string> missions;
        for (auto& n : fs.sim().names())
            if (extension(n) == "ann") {
                std::string b = upper(n.substr(n.find_last_of("/\\") == std::string::npos ? 0 : n.find_last_of("/\\") + 1));
                b = b.substr(0, b.size() - 4);
                if (want == "ALL" || b == want) missions.push_back(b);
            }
        std::error_code ec;
        for (auto& e : std::filesystem::directory_iterator(std::string(argv[1]) + "/Mission", ec))
            if (extension(e.path().filename().string()) == "ann") {
                std::string b = upper(e.path().stem().string());
                if (want == "ALL" || b == want) missions.push_back(b);
            }
        std::sort(missions.begin(), missions.end());
        missions.erase(std::unique(missions.begin(), missions.end()), missions.end());
        TextureAlpha alpha(fs);
        struct Shapes { CollModel oldS, newS; bool ok = false; };
        std::map<std::string, std::unique_ptr<Shapes>> cache;
        auto shapes = [&](std::string key) -> Shapes* {
            key = upper(key);
            while (!key.empty() && key[0] == '@') key.erase(0, 1);
            auto& p = cache[key];
            if (!p) {
                p = std::make_unique<Shapes>();
                Bytes b;
                Model mdl;
                if (fs.read(key, b) && loadModel(b, mdl)) {
                    legacyCollision(mdl, p->oldS);
                    buildCollision(mdl, alpha.forModel(mdl), p->newS);
                    p->ok = true;
                }
            }
            return p->ok ? p.get() : nullptr;
        };
        auto footprint = [](const CollShape& s, float minY, float maxY) {  // summed box footprint in a height band
            float a = 0;
            for (auto& b : s.boxes)
                if (b.hi.y > minY && b.lo.y < maxY) a += (b.hi.x - b.lo.x) * (b.hi.z - b.lo.z);
            return a;
        };
        int total = 0, flagged = 0, oldBoxes = 0, newBoxes = 0;
        double oldArea = 0, newArea = 0;
        for (auto& mn : missions) {
            Bytes b;
            Mission m;
            std::string err;
            if (!fs.read("mission/" + mn + ".ann", b) || !loadMission(b, m, &err)) continue;
            std::map<std::string, int> seen;  // one line per model and reason per mission
            for (size_t i = 0; i < m.objects.size(); i++) {
                const auto& o = m.objects[i];
                if (o.defIndex < 0 || size_t(o.defIndex) >= m.defs.size()) continue;
                const MissionDef& d = m.defs[size_t(o.defIndex)];
                if (d.firstModel.empty() || !d.animScript.empty()) continue;  // rigged units carry no boxes
                Shapes* s = shapes(d.firstModel);
                if (!s) continue;
                total++;
                bool marker = o.type == 0x162 || o.type == 0x61 || (o.type >= 0x56 && o.type <= 0x59) || o.type == 0x5c || o.isLight;
                const CollShape& oldC = s->oldS.full;
                static const CollShape none;
                const CollShape& newC = marker ? none : o.type == 0x47 ? s->newS.trunk : s->newS.full;
                oldBoxes += int(oldC.boxes.size()), newBoxes += int(newC.boxes.size());
                float base = s->oldS.meshLo.y;
                float oldFoot = footprint(oldC, base + 1.2f, base + 7.f), newFoot = footprint(newC, base + 1.2f, base + 7.f);
                oldArea += oldFoot, newArea += newFoot;
                std::string why;
                if (marker && !oldC.boxes.empty()) why += o.type == 0x162 || o.type == 0x61 ? " no-collision-class" : " marker";
                if (oldFoot > 40.f && oldFoot > 3.f * std::max(1.f, newFoot)) why += " oversized";
                if (!oldC.boxes.empty() && newC.boxes.empty() && !marker) why += " no-solid";
                if (s->newS.samples > 0 && s->newS.transparentSamples > s->newS.samples / 4) why += " alpha-cutout";
                if (verbose && s->newS.sliverTris > 0) why += " degenerate-tris";
                if (verbose && s->newS.decalMeshes > 0) why += " decal-meshes";
                if (why.empty()) continue;
                flagged++;
                std::string k = d.firstModel + why;
                if (seen[k]++ && !verbose) continue;
                Vec3 sz = s->oldS.meshHi - s->oldS.meshLo;
                printf("%-4s obj %4zu type %3x %-14s%-28s mesh %3.0fx%2.0fx%3.0f  old %2zu boxes, %5.0f m2 at gear height -> new %3zu boxes, %5.0f m2 (alpha %d%%)\n",
                       mn.c_str(), i, o.type, d.firstModel.c_str(), why.c_str(), sz.x, sz.y, sz.z, oldC.boxes.size(), oldFoot, newC.boxes.size(),
                       newFoot, s->newS.samples ? 100 * s->newS.transparentSamples / s->newS.samples : 0);
            }
            for (auto& [k, n] : seen)
                if (n > 1 && !verbose) printf("%-4s      (x%d) %s\n", mn.c_str(), n, k.c_str());
        }
        printf("%zu missions, %d static objects, %d flagged; boxes per object %.1f -> %.1f; blocking footprint at gear height %.0f -> %.0f m2\n",
               missions.size(), total, flagged, total ? double(oldBoxes) / total : 0., total ? double(newBoxes) / total : 0., oldArea, newArea);
        return 0;
    }
    if (cmd == "paths" && argc >= 4) {  // list a mission's paths
        Bytes b;
        Mission m;
        std::string err;
        if (!fs.read("mission/" + std::string(argv[3]) + ".ann", b) || !loadMission(b, m, &err)) return 1;
        for (auto& p : m.paths) {
            printf("%s anchor %.0f,%.0f,%.0f %zu points:", p.name.c_str(), p.anchor[0], p.anchor[1], p.anchor[2], p.points.size());
            for (size_t i = 0; i < p.points.size(); i++)
                if (argc > 4 || i < 3 || i + 3 >= p.points.size()) printf(" %.0f,%.0f", p.points[i][0], p.points[i][2]);
            printf("\n");
        }
        return 0;
    }
    if (cmd == "scripts" && argc >= 4) {  // run a mission's scripts headless
        Bytes b;
        Mission m;
        std::string err, mn = argv[3];
        if (!fs.read("mission/" + mn + ".ann", b) || !loadMission(b, m, &err)) {
            fprintf(stderr, "mission: %s\n", err.c_str());
            return 1;
        }
        ScriptWorld world;
        for (auto& o : m.objects) world.unitNames.push_back(o.name), world.unitTeams.push_back(o.team);
        ScriptSystem sys(fs, world);
        sys.verbose = argc >= 6;
        sys.load(m, &err);
        float secs = argc >= 5 ? float(atof(argv[4])) : 10.f;
        for (int f = 0; f < int(secs * 30); f++) sys.update(1.f / 30.f);
        printf("%s: %d script instances, %llu instructions in %.0fs\n", mn.c_str(), sys.vmCount(),
               (unsigned long long)sys.instructions(), secs);
        for (auto& s2 : sys.missing) printf("  missing builtin %s\n", s2.c_str());
        for (auto& s2 : sys.calledNatives(argc < 6)) printf("  %s builtin %s\n", ScriptVM::isBound(s2.substr(0, s2.find('/')), atoi(s2.c_str() + s2.find('/') + 1)) ? "called" : "unbound", s2.c_str());
        return 0;
    }
    fprintf(stderr, "unknown command\n");
    return 1;
}
