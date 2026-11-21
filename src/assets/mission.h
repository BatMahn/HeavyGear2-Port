// Compiled mission (.ANN) loader — FUN_1008dba0 / FUN_10085570.
#pragma once
#include <array>
#include <memory>
#include <string>
#include <vector>

#include "../core/fs.h"

namespace hg2 {

struct MissionObject {
    bool userHeader = false;  // 0x3a (player / user) vs 0x3b
    std::string name, script, walkGraph;
    int type = 0;        // object class keyword (0x46 Regular_Object, 0x48 Bipeded_Object, ...)
    int defIndex = -1;   // index into Mission::defs
    int team = 99;       // 99 = No_Team, 100.. = Team_1..
    double pos[3] = {};
    double rot[3] = {};  // degrees
    double quat[4] = {};
    bool hasQuat = false;
    bool isLight = false;             // 0x43 Light_Information (object type 0x5c)
    uint8_t lightColor[3] = {255, 255, 255};
    float lightRange = 0, lightIntensity = 1;
};

struct MissionDef {
    const uint8_t* data = nullptr;
    size_t size = 0;
    std::string firstModel;  // first ".m" geometry referenced (render bring-up)
    std::string animScript;  // ".ECY" animation state machine, if any
};

struct MissionPath {
    std::string name;
    float anchor[3] = {0, 0, 0};
    std::vector<std::array<float, 3>> points;  // world-space waypoints (anchor + control points, beziers sampled)
};

// FountainSystem (0x12a): unit generators. Templates name a definition + object type (+ script); each fountain
// spawns from a list of templates at its position (binary reader FUN_1005e8c0, generator FUN_1005bb40, template
// FUN_1005e7a0; generator object FUN_1005b020). Field offsets are the generator's in the original.
struct FountainTemplate { int defIndex = -1, type = 0x48; std::string script; };
struct Fountain {
    std::string name, script;
    int team = 101;            // 0x6c (mission team numbering; +0xc holds team - 99)
    bool active = true;        // 0x132 == 3 (+0x14)
    int mode = 0;              // 0x12f (0..2, +0x2e; every mode spawns the same way)
    int total = 0;             // 0x130 (+0x28) units it may lose before it switches off (0 = no limit)
    int maxAlive = 0;          // 0x12b (+0x2c, 1..16) of its units in the field at once (0 = no limit)
    int wave = 1;              // 0x131 (+0x2a, 1..8) units per spawn, all from one template
    bool minRange = true;      // +0x19 (byte 1 of 0x134): only spawn beyond range[0] of the user
    float pos[3] = {0, 0, 0}, rot[3] = {0, 0, 0};
    float range[3] = {400, 500, 580};  // 0x15e min / 0x15f max spawn distance, 0x160 recycle distance
    std::vector<int> templates;        // 0x135 indices into Mission::fountainTemplates (up to 16, no repeats)
};

// Surfaces (0xd2): terrain surface types — colour key, friction, damage per second (lava), footstep sound.
struct Surface {
    int index = 0; uint8_t color[3] = {0, 0, 0}; float friction = 1; int damage = 0; std::string footstep;
    float movementCost = 0;  // 0x154 (entry +0x44)
    uint32_t flags = 0;      // 0x153 (entry +0x48): bits 2-5 tell the pathfinder 1 open / 2 blocked (FUN_100c2520)
    uint32_t dustColor = 0;  // 0x156 DustColor as 0x00RRGGBB (tints SPARK0 / SPARK1 / SPARKU)
    int dustIndex = 0;       // 0x165 DustIndex: bullet ground hits use SPARK0 (0), SPARK1 (1), else SPARKO
};

struct Mission {
    Bytes raw;
    std::string title, textFile, terrain, skyTexture, skyModel, waterTexture, msgFile;
    float fogMin = 0, fogMax = 0, fogIntensity = 0;
    uint8_t fog[4] = {0, 0, 0, 0};
    int clipping = 0;
    int musicTrack = 0;  // RedBook_Track (0x39): CD audio track for the mission, 0 if none
    float lightAmbient = 0.3f, lightDiffuse = 0.5f, lightDir[3] = {0, -1, 0};
    uint8_t lightColor[3] = {255, 255, 255};
    bool hasWater = false;
    float waterLevel = 0, waterTidal = 0, waterAnim = 0;
    int waterAlpha = 255;
    std::vector<std::string> scriptProjects, scriptLibraries, scriptFiles;
    std::vector<MissionDef> defs;
    std::vector<std::shared_ptr<Bytes>> extraDefData;  // definitions added after loading (shell gear choices)
    int addDef(const uint8_t* data, size_t size);       // copies the block; returns its def index
    std::vector<MissionObject> objects;
    std::vector<MissionPath> paths;
    std::vector<FountainTemplate> fountainTemplates;
    std::vector<Fountain> fountains;
    // FountainSystem scalars (FUN_1005e8c0): 0x132 == 3 switches the system on (+0x15, off by default), 0x12b units in
    // each template's pool and the most fountain units alive at once (+0x1c, default 4), 0x12c the most units the
    // system makes over the mission, counting the lost (+0x18, 0 = no limit)
    bool fountainsOn = false;
    int fountainPool = 4, fountainCap = 0;
    std::vector<Surface> surfaces;
    const MissionPath* path(const std::string& name) const;
};

bool loadMission(const Bytes& data, Mission& out, std::string* err = nullptr);

}  // namespace hg2
