// hg2extract — inspect / extract Heavy Gear II data.
//   hg2extract <datadir> list                  list archive entries
//   hg2extract <datadir> get <name> <out>      extract one file (archives, loose, or ~-compressed)
//   hg2extract <datadir> check                 parse every model / terrain / mission and report
#include <cstdio>
#include <string>

#include "../src/assets/assets.h"
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
        return 0;
    }
    fprintf(stderr, "unknown command\n");
    return 1;
}
