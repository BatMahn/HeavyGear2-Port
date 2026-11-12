// One mission in play: loads the mission with the shell's choices (the user's chassis and variant, squad mates,
// instant-action waves), runs the simulation, HUD and scripts until MissionOver, an abort or the window closing.
#pragma once
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "../app.h"

namespace hg2 {
class NetGame;

struct GearChoice {
    int chassis = -1;                              // MstrGear.bin entry (-1: the mission's own gear)
    std::vector<std::pair<int, int>> weapons;      // (hardpoint, MstrWpns.bin index); empty: the chassis default
    int armor = 0, fireControl = 0;                // the variant's stats (armor 0: the chassis' own)
    bool hasSystems = false;                       // the variant's sensors, manoeuvre, perks and flaws follow
    int sensors = 0, maneuver = 0;
    uint32_t perks = 0, flaws = 0;
};
struct SquadMate {
    std::string name;
    GearChoice gear;
};

struct MissionSetup {
    std::string mission;           // "S01"
    std::string title;             // shown at the start (the shell's mission title); empty: the mission file's
    GearChoice player;
    std::vector<SquadMate> squad;  // AI squad mates following the user
    // instant action: waves of `enemies` hunting the user; success when every wave is destroyed
    std::vector<SquadMate> enemies;
    int waves = 0;
    float difficulty = 0.5f;       // 0 easiest .. 1 hardest: enemy accuracy / alertness
    bool space = false;            // an asteroid / station mission (the briefing's space flag): low gravity
    NetGame* net = nullptr;  // network match (instant-action arena, players from the lobby)
};

// Offscreen test options (command line).
struct TestOptions {
    std::string screenshot;
    bool camSet = false, noFog = false, verboseScripts = false;
    float cam[5] = {0, 0, 0, 0, 0};  // x, y, z, yaw deg, pitch deg
    int driveFrames = 0, fireFrames = 0;
    bool navDrive = false, external = false, sideView = false;
    std::vector<std::pair<int, std::string>> inputScript;
    float startX = -1, startZ = -1, startYaw = 0;
};

struct MissionResult {
    enum Outcome { Failure = 0, Success = 1, Alternate = 2, Aborted = 3, Quit = 4, Error = 5 } outcome = Error;
    float time = 0;
    int kills = 0, shots = 0, hits = 0;
    std::vector<std::string> squadLost;  // squad mates destroyed
    std::string error;
};

MissionResult runMission(AppContext& app, const MissionSetup& setup, const TestOptions* test = nullptr);

// MstrGear.bin entry `index` (definition block, same format as a mission's gear definitions).
bool masterGear(const FileSystem& fs, int index, Bytes& out);
bool writePngFile(const std::string& path, const std::vector<uint8_t>& rgba, int w, int h);

}  // namespace hg2
