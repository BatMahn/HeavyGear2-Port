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
// spawns from a list of templates at its position (parser FUN_1005bc?? / text form FUN_1005b4a0).
struct FountainTemplate { int defIndex = -1, type = 0x48; std::string script; };
struct Fountain {
    std::string name, script;
    int team = 101;            // 0x6c (mission team numbering)
    bool active = true;        // 0x132 == 3
    int mode = 0;              // 0x12f (0..2)
    int total = 0;             // 0x130 units it can make (0 = no limit)
    int maxAlive = 1;          // 0x131 (1..8) at once
    int wave = 1;              // 0x12b (1..16)
    uint32_t flags = 0;        // 0x134
    float pos[3] = {0, 0, 0}, rot[3] = {0, 0, 0};
    float range[3] = {400, 500, 580};  // 0x15e / 0x15f / 0x160
    std::vector<int> templates;        // 0x135 indices into Mission::fountainTemplates
};

// Surfaces (0xd2): terrain surface types — colour key, friction, damage per second (lava), footstep sound.
struct Surface { int index = 0; uint8_t color[3] = {0, 0, 0}; float friction = 1; int damage = 0; std::string footstep; };

struct Mission {
    Bytes raw;
    std::string title, textFile, terrain, skyTexture, skyModel, waterTexture, msgFile;
    float fogMin = 0, fogMax = 0, fogIntensity = 0;
    uint8_t fog[4] = {0, 0, 0, 0};
    int clipping = 0;
    int musicTrack = 0;  // RedBook_Track (0x39): CD audio track for the mission, 0 if none
    float lightAmbient = 0.3f, lightDiffuse = 0.7f, lightDir[3] = {0, -1, 0};
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
    std::vector<Surface> surfaces;
    const MissionPath* path(const std::string& name) const;
};

bool loadMission(const Bytes& data, Mission& out, std::string* err = nullptr);

}  // namespace hg2
