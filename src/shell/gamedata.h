// Shell databases from hg2shell.prj: mission briefings (.MSN), the gear inventory (GRINVTRY.TXT: chassis,
// their variants and pictures), gear variants (.VAR loadouts), the weapon inventory (WPINVTRY.TXT, in
// MstrWpns.bin order), pilots (GAMECHRS.TXT + PIBIO*.CHR) and the plain text lists. All are serialized
// shell objects: u32 CRC, 'b', then length-prefixed strings and u32 fields.
#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>

#include "../core/fs.h"

namespace hg2 {

// Mission briefing (FUN_100d26be). MissionOver codes: 1 success -> next, 0 failure -> retry, 2 -> alternate.
struct MsnInfo {
    std::string id, title, ann;
    std::string retryId, nextId, altId;
    std::string forcedChassis, forcedVariant;  // training missions that set the user's gear
    int squadMax = 1;              // gears including the user
    bool fixedSquad = false;       // no pilot list: historical / training squads
    std::vector<std::string> pilots;  // pilots available for the squad
    std::vector<int> pilotRequired;   // 1: pilot is always in the squad
    bool space = false;
    int threatMin = 0, threatMax = 0;
    std::string introMovie, outroMovie;  // full-screen movies before / after (Setup\movies)
    std::string movie, image;            // briefing video (Shell\movies) and still
    std::string briefing, success, failure, alternate;
};
bool parseMsn(const Bytes& b, MsnInfo& out);

struct Variant { std::string name, file; bool hidden = false; };
struct Chassis {
    std::string name, stf, picture, description;
    int index = 0;        // MstrGear.bin entry
    int weightClass = 1;  // 1 light .. 4 assault
    bool squad = false;   // Black Talon (registration) gears
    std::vector<Variant> variants;
};
struct Pilot {
    std::string name, fullName, chr, voice, portrait;  // portrait "SPPXKAGU.TGA" (U/M/D states)
    std::string chassis, variant, bio;                  // favourite gear
};
struct VarWeapon { int slot; std::string wpn; int tableId = -1; float ammo = 0; };  // hardpoint (1..9), weapon file
                                                                                     // (index in WPINVTRY), WPNTABLE id, rounds
// A variant's construction record (.VAR, FUN_100c5420): weapons plus the stats and perk / flaw bit fields that the
// threat value is computed from.
struct VarRecord {
    std::vector<VarWeapon> weapons;
    int sensors = 0, fireControl = 0, armor = 0, size = 0, maneuver = 0, move1 = 0, move2 = 0;  // +0xf4 .. +0x10c
    uint32_t perks = 0, flaws = 0;  // +0x110, +0x114 bit fields
};
struct Threat { int total = 0, offensive = 0, defensive = 0, misc = 0; };

class GameData {
public:
    bool load(const FileSystem& fs);
    const FileSystem* fs = nullptr;
    std::vector<Chassis> chassis;
    std::vector<Pilot> pilots;
    std::vector<std::string> weaponFiles;  // MstrWpns order
    std::vector<std::string> iaMaps, iaNames;  // instant action arenas: mission ids and names
    const Chassis* findChassis(const std::string& name) const;
    const Pilot* findPilot(const std::string& name) const;
    bool msn(const std::string& id, MsnInfo& out) const;
    bool variant(const std::string& file, std::vector<VarWeapon>& out) const;  // VAR loadout
    bool variantRecord(const std::string& file, VarRecord& out) const;
    // Threat value (FUN_100bf7bf) of a variant record; `weapons` (hardpoint, WPINVTRY index) replaces its loadout
    // when given (gear lab refits).
    Threat threat(const VarRecord& v) const;
    // The perks and flaws set in a record's bit fields (PERKSTBL names, with their levels).
    void perksAndFlaws(const VarRecord& v, std::vector<std::string>& perks, std::vector<std::string>& flaws) const;
    // The perk / flaw bit fields as FUN_100c00aa reads them: {word (0 = +0x110, 1 = +0x114), shift, width, perk id};
    // ids 35 and up are flaws.
    struct PerkField { int word, shift, width, id; };
    static const std::vector<PerkField>& perkFields();
    std::string perkLabel(int id) const;
    VarRecord withWeapons(const VarRecord& v, const std::vector<std::pair<int, int>>& weapons) const;
    // gear-lab tables (hg2shell.prj)
    struct WpnCost { std::string name; int base = 0; float perRound = 0; };
    std::vector<WpnCost> wpnTable;                // WPNTABLE, by weapon id
    std::vector<std::string> perkExpr, perkName;  // PERKSTBL, by perk id
    int tgtLo = 0, mnvLo = 0;
    std::vector<float> tgtMul, mnvMul;            // TGTMLTBL (fire control), MNVMLTBL (manoeuvre)
    std::map<std::string, std::pair<int, float>> wpnInfo;  // .WPN file -> WPNTABLE id, stock rounds
    float perkCost(int id, float R, float h, int S, int crew, int passengers, bool limited, int wpnCount) const;
    int weaponIndex(const std::string& wpn) const;
    std::string text(const std::string& file) const;  // a plain text file ("" if missing)
    bool hasMovie(const std::string& name) const;
};

}  // namespace hg2
