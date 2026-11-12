#include "gamedata.h"

#include <SDL3/SDL.h>

#include <cmath>
#include <cstring>
#include <sstream>

namespace hg2 {

namespace {
struct Cur {
    const Bytes& b;
    size_t p;
    bool bad = false;
    Cur(const Bytes& bb, size_t start) : b(bb), p(start) {}
    uint32_t u32() {
        if (p + 4 > b.size()) {
            bad = true, p = b.size();
            return 0;
        }
        uint32_t v;
        memcpy(&v, &b[p], 4);
        p += 4;
        return v;
    }
    uint8_t u8() {
        if (p >= b.size()) {
            bad = true;
            return 0;
        }
        return b[p++];
    }
    std::string str() {
        uint32_t n = u32();
        if (bad || n > b.size() - p) {
            bad = true, p = b.size();
            return std::string();
        }
        std::string s(reinterpret_cast<const char*>(&b[p]), n);
        p += n;
        while (!s.empty() && s.back() == 0) s.pop_back();
        return s;
    }
};
bool header(const Bytes& b) { return b.size() > 5 && b[4] == 'b'; }
std::string stripExt(std::string s) {
    size_t d = s.rfind('.');
    return d == std::string::npos ? s : s.substr(0, d);
}
}  // namespace

bool parseMsn(const Bytes& b, MsnInfo& m) {
    if (!header(b)) return false;
    Cur c(b, 5);
    m.title = c.str(), m.ann = c.str();
    m.retryId = c.str(), m.nextId = c.str(), m.altId = c.str();
    m.forcedChassis = c.str(), m.forcedVariant = c.str();
    m.squadMax = int(c.u32());
    c.u32();
    m.fixedSquad = c.u32() != 0;
    if (!m.fixedSquad) {
        uint32_t n = c.u32();
        for (uint32_t i = 0; i < n && !c.bad && i < 64; i++) m.pilots.push_back(c.str());
    }
    uint32_t n = c.u32();
    for (uint32_t i = 0; i < n && !c.bad && i < 64; i++) m.pilotRequired.push_back(int(c.u32()));
    c.u32();
    m.space = c.u32() != 0;
    m.threatMin = int(c.u32()), m.threatMax = int(c.u32());
    m.introMovie = c.str();
    c.u32(), c.u32();
    m.outroMovie = c.str();
    c.u32(), c.u32();
    m.movie = c.str(), m.image = c.str();
    m.briefing = c.str(), m.success = c.str(), m.failure = c.str(), m.alternate = c.str();
    m.id = stripExt(m.ann);
    return !c.bad;
}

bool GameData::load(const FileSystem& f) {
    fs = &f;
    Bytes b;
    // gear inventory
    if (f.read("GRINVTRY.TXT", b) && header(b)) {
        Cur c(b, 5);
        c.str();
        uint32_t n = c.u32();
        for (uint32_t i = 0; i < n && !c.bad && i < 512; i++) {
            Chassis ch;
            ch.index = int(i);
            ch.name = c.str(), ch.stf = c.str();
            c.u32(), c.u32();
            ch.squad = c.u32() == 1;
            ch.weightClass = int(c.u32());
            c.u8(), c.u8();
            ch.picture = c.str(), ch.description = c.str();
            uint32_t nv = c.u32();
            for (uint32_t k = 0; k < nv && !c.bad && k < 64; k++) {
                Variant v;
                v.hidden = c.u8() != 0;
                c.u8();
                v.name = c.str(), v.file = c.str();
                ch.variants.push_back(v);
            }
            if (!c.bad) chassis.push_back(ch);
        }
    }
    // weapon inventory: count then the .WPN file names in MstrWpns.bin order
    if (f.read("WPINVTRY.TXT", b) && header(b)) {
        Cur c(b, 5);
        c.u32();
        uint32_t n = c.u32();
        for (uint32_t i = 0; i < n && !c.bad && i < 1024; i++) weaponFiles.push_back(upper(c.str()));
    }
    // gear-lab tables
    if (f.read("WPNTABLE.TXT", b) && header(b)) {
        Cur c(b, 5);
        for (int i = 0; i < 0x5a && !c.bad; i++) {
            int id = int(c.u32());
            WpnCost w;
            w.name = c.str();
            w.base = int(c.u32());
            uint32_t raw = c.u32();
            memcpy(&w.perRound, &raw, 4);
            c.u32(), c.u32();
            if (id >= 0 && id < 0x5a && !c.bad) {
                if (wpnTable.size() <= size_t(id)) wpnTable.resize(size_t(id) + 1);
                wpnTable[size_t(id)] = w;
            }
        }
    }
    if (f.read("PERKSTBL.TXT", b) && header(b)) {
        Cur c(b, 5);
        while (c.p + 12 <= b.size() && !c.bad) {
            int id = int(c.u32());
            std::string n = c.str(), e = c.str();
            if (c.bad || id < 0 || id > 255) break;
            if (perkExpr.size() <= size_t(id)) perkExpr.resize(size_t(id) + 1), perkName.resize(size_t(id) + 1);
            perkExpr[size_t(id)] = e, perkName[size_t(id)] = n;
        }
    }
    auto floats = [&](const char* name, int& lo, std::vector<float>& out) {
        if (!f.read(name, b) || !header(b)) return;
        Cur c(b, 5);
        lo = int(c.u32());
        int hi = int(c.u32());
        for (int i = lo; i <= hi && !c.bad && i - lo < 256; i++) {
            uint32_t raw = c.u32();
            float v;
            memcpy(&v, &raw, 4);
            if (!c.bad) out.push_back(v);
        }
    };
    floats("TGTMLTBL.TXT", tgtLo, tgtMul);
    floats("MNVMLTBL.TXT", mnvLo, mnvMul);
    for (auto& ch : chassis)  // weapon file -> table id and stock rounds, from the stock variants
        for (auto& v : ch.variants) {
            VarRecord r;
            if (!variantRecord(v.file, r)) continue;
            for (auto& w : r.weapons)
                if (w.tableId >= 0 && !wpnInfo.count(w.wpn)) wpnInfo[w.wpn] = {w.tableId, w.ammo};
        }
    // pilots
    if (f.read("GAMECHRS.TXT", b) && header(b)) {
        Cur c(b, 5);
        c.u32();
        uint32_t n = c.u32();
        for (uint32_t i = 0; i < n && !c.bad && i < 64; i++) {
            Pilot p;
            p.name = c.str();
            c.u8();
            p.chr = c.str();
            Bytes cb;
            if (f.read(p.chr + ".CHR", cb) && header(cb)) {
                Cur d(cb, 5);
                p.fullName = d.str();
                for (int k = 0; k < 7; k++) d.u32();
                p.voice = d.str(), p.portrait = d.str(), p.chassis = d.str(), p.variant = d.str(), p.bio = d.str();
            }
            pilots.push_back(p);
        }
    }
    // instant action arenas
    std::string names = text("INSTACTN.TXT"), maps = text("INSTCTNF.TXT");
    auto lines = [](const std::string& s, bool words) {
        std::vector<std::string> out;
        std::string cur;
        for (char ch : s) {
            bool sep = ch == '\n' || ch == '\r' || ch == 0 || (words && (ch == ' ' || ch == '\t'));
            if (sep) {
                while (!cur.empty() && cur.back() == ' ') cur.pop_back();
                if (!cur.empty()) out.push_back(cur);
                cur.clear();
            } else {
                cur += ch;
            }
        }
        while (!cur.empty() && cur.back() == ' ') cur.pop_back();
        if (!cur.empty()) out.push_back(cur);
        return out;
    };
    iaNames = lines(names, false);
    for (auto& m : lines(maps, true)) iaMaps.push_back(upper(stripExt(m)));
    SDL_Log("shell data: %zu chassis, %zu pilots, %zu weapons, %zu arenas", chassis.size(), pilots.size(), weaponFiles.size(),
            iaMaps.size());
    return !chassis.empty();
}

const Chassis* GameData::findChassis(const std::string& name) const {
    for (auto& c : chassis)
        if (lower(c.name) == lower(name)) return &c;
    return nullptr;
}

const Pilot* GameData::findPilot(const std::string& name) const {
    for (auto& p : pilots)
        if (lower(p.name) == lower(name)) return &p;
    return nullptr;
}

bool GameData::msn(const std::string& id, MsnInfo& out) const {
    Bytes b;
    if (!fs->read(upper(id) + ".MSN", b)) return false;
    out = MsnInfo{};
    if (!parseMsn(b, out)) return false;
    if (out.id.empty()) out.id = upper(id);
    out.id = upper(id);
    return true;
}

// .VAR: XOR-scrambled (key index = remaining length % 7, FUN_1006a271): chassis, variant name, weapon count,
// u32, then {u8 used, u32 hardpoint keyword 0xa4.., u32, u32 class keyword, .WPN file, f32} per weapon.
bool GameData::variantRecord(const std::string& file, VarRecord& out) const {
    Bytes b;
    std::string fn = upper(stripExt(file)) + ".VAR";
    if (!fs->read(fn, b) || !header(b)) return false;
    static const uint8_t key[7] = {0x23, 0x22, 0x70, 0x97, 0x7b, 0x03, 0x19};
    Bytes d(b.begin() + 9, b.end());
    size_t n = d.size();
    for (size_t i = 0; i < n; i++) d[i] ^= key[(n - i) % 7];
    Cur c(d, 0);
    c.str(), c.str();
    c.u32(), c.u32();  // weapon count (recounted from the used flags), u32
    out = VarRecord{};
    for (int i = 0; i < 7 && !c.bad; i++) {  // always seven slots
        uint8_t used = c.u8();
        uint32_t slot = c.u32();
        int id = int(c.u32());
        c.u32();
        std::string w = c.str();
        uint32_t raw = c.u32();
        float rounds;
        memcpy(&rounds, &raw, 4);
        if (used && !w.empty() && slot >= 0xa4 && slot <= 0xac) out.weapons.push_back({int(slot - 0xa4 + 1), upper(w), id, rounds});
    }
    int* st[7] = {&out.sensors, &out.fireControl, &out.armor, &out.size, &out.maneuver, &out.move1, &out.move2};
    for (int* p : st) *p = int(c.u32());
    out.perks = c.u32(), out.flaws = c.u32();
    return !c.bad;
}

bool GameData::variant(const std::string& file, std::vector<VarWeapon>& out) const {
    VarRecord r;
    if (!variantRecord(file, r)) return false;
    out = r.weapons;
    return true;
}

VarRecord GameData::withWeapons(const VarRecord& v, const std::vector<std::pair<int, int>>& weapons) const {
    VarRecord r = v;
    r.weapons.clear();
    for (auto& [slot, wi] : weapons) {
        if (wi < 0 || size_t(wi) >= weaponFiles.size()) continue;
        VarWeapon w{slot, weaponFiles[size_t(wi)]};
        auto it = wpnInfo.find(w.wpn);
        if (it != wpnInfo.end()) w.tableId = it->second.first, w.ammo = it->second.second;
        r.weapons.push_back(w);
    }
    return r;
}

// Perk / flaw cost: PERKSTBL expressions are postfix token lists (FUN_100e1017): numbers, the gear's values
// (R rating, H_Range, Size, Crew, Passenger, Limited, WpnCount, ...), + * -, RndDown / RndUp (truncate) and
// "a b c if" (b unless a is 0, then c). Parentheses and commas only group.
float GameData::perkCost(int id, float R, float h, int S, int crew, int passengers, bool limited, int wpnCount) const {
    if (id < 0 || size_t(id) >= perkExpr.size()) return 0;
    std::vector<float> st;
    std::string e = perkExpr[size_t(id)];
    for (auto& ch : e)
        if (ch == '(' || ch == ')' || ch == ',' || ch == '\t') ch = ' ';
    std::istringstream in(e);
    std::string t;
    auto pop = [&]() { float v = st.empty() ? 0.f : st.back(); if (!st.empty()) st.pop_back(); return v; };
    while (in >> t) {
        std::string u = lower(t);
        if (u == "r") st.push_back(R);
        else if (u == "h_range") st.push_back(h);
        else if (u == "r_range" || u == "d_arc" || u == "o_arc" || u == "wpnlink" || u == "armcount") st.push_back(0);
        else if (u == "wpncount") st.push_back(float(wpnCount));
        else if (u == "size") st.push_back(float(S));
        else if (u == "crew") st.push_back(float(crew));
        else if (u == "passenger") st.push_back(float(passengers));
        else if (u == "limited") st.push_back(limited ? 1.f : 0.f);
        else if (u == "full") st.push_back(0);
        else if (u == "rnddown" || u == "rndup") st.push_back(std::trunc(pop()));
        else if (u == "+") { float b = pop(), a = pop(); st.push_back(a + b); }
        else if (u == "*") { float b = pop(), a = pop(); st.push_back(a * b); }
        else if (u == "-") { float b = pop(), a = pop(); st.push_back(a - b); }
        else if (u == "if") { float no = pop(), yes = pop(), cond = pop(); st.push_back(cond == 0.f ? no : yes); }
        else st.push_back(float(atof(t.c_str())));
    }
    return st.empty() ? 0.f : st.back();
}

const std::vector<GameData::PerkField>& GameData::perkFields() {
    static const std::vector<PerkField> fields = {
        {0, 0, 1, 1},   {0, 1, 1, 2},   {0, 2, 1, 3},   {0, 3, 1, 4},   {0, 4, 1, 5},   {0, 5, 1, 6},   {0, 6, 2, 7},
        {0, 8, 1, 8},   {0, 9, 1, 9},   {0, 10, 3, 10}, {0, 13, 3, 11}, {0, 16, 1, 12}, {0, 17, 1, 14}, {0, 18, 4, 15},
        {0, 22, 2, 16}, {0, 24, 2, 21}, {0, 26, 1, 22}, {0, 27, 2, 23}, {1, 0, 2, 24},  {1, 2, 2, 24},  {1, 4, 2, 24},
        {1, 6, 2, 24},  {1, 8, 1, 25},  {1, 9, 1, 26},  {0, 30, 1, 27}, {0, 31, 1, 29}, {0, 29, 1, 30}, {1, 10, 3, 31},
        {1, 13, 1, 32}, {1, 14, 1, 34}, {1, 15, 2, 35}, {1, 17, 2, 36}, {1, 19, 1, 37}, {1, 20, 1, 39}, {1, 21, 1, 38},
        {1, 22, 1, 40}, {1, 23, 1, 42}, {1, 24, 1, 43}, {1, 25, 1, 45}, {1, 26, 1, 49}, {1, 27, 1, 50}, {1, 28, 1, 51},
        {1, 29, 1, 51}, {1, 30, 1, 51}, {1, 31, 1, 51}};
    return fields;
}

std::string GameData::perkLabel(int id) const {
    std::string n = size_t(id) < perkName.size() && !perkName[size_t(id)].empty() ? perkName[size_t(id)] : "Perk " + std::to_string(id);
    for (auto& ch : n)
        if (ch == '_') ch = ' ';
    if (id == 16) n = "HEP (environment)";
    return n;
}

void GameData::perksAndFlaws(const VarRecord& v, std::vector<std::string>& perks, std::vector<std::string>& flaws) const {
    const auto& fields = perkFields();
    std::map<int, int> level;  // id -> summed level (reinforced armour / weak facing per facing)
    std::vector<int> order;
    for (auto& f : fields) {
        uint32_t w = f.word ? v.flaws : v.perks;
        int lv = int(w >> f.shift & ((1u << f.width) - 1));
        if (!lv) continue;
        if (!level.count(f.id)) order.push_back(f.id);
        level[f.id] += lv;
    }
    for (int id : order) {
        std::string n = perkLabel(id);
        if (level[id] > 1) n += " " + std::to_string(level[id]);
        (id >= 35 ? flaws : perks).push_back(n);
    }
}

// FUN_100bf7bf: offensive = (weapon base costs + manipulator arms + rounds x per-round cost) x fire-control
// multiplier; defensive = (armour^2 + 0.013824 x max(MP)^3 + min(MP)^2) x manoeuvre multiplier; miscellaneous =
// 103 + max(0, sensors + 1 + perks - flaws)^2; total = the average of the three (each at least 1), rounded up.
Threat GameData::threat(const VarRecord& v) const {
    auto tbl = [](const std::vector<float>& t, int lo, int i) { return i - lo >= 0 && size_t(i - lo) < t.size() ? t[size_t(i - lo)] : 0.f; };
    uint32_t P = v.perks, F = v.flaws;
    double base = 0, rounds = 0;
    int wcount = 0;
    for (auto& w : v.weapons) {
        if (w.tableId < 0 || size_t(w.tableId) >= wpnTable.size()) continue;
        base += wpnTable[size_t(w.tableId)].base;
        rounds += double(w.ammo) * wpnTable[size_t(w.tableId)].perRound;
        wcount++;
    }
    double arms = double(v.size) + 2.0 * double((P >> 27) & 3) - 2.0;  // FUN_100bfc0f
    double off = (base + arms * arms * 0.25 * 2.0 + rounds) * tbl(tgtMul, tgtLo, v.fireControl);
    double mx = std::max(v.move1, v.move2), mn = std::min(v.move1, v.move2);
    double def = (double(v.armor) * v.armor + mx * mx * mx * 0.013824 + mn * mn) * tbl(mnvMul, mnvLo, v.maneuver);
    // FUN_100c00aa: perks minus flaws
    auto pc = [&](int id, float R = 0, float h = 0, int S = 0, int crew = 0, int pass = 0, bool lim = false, int wc = 0) {
        return perkCost(id, R, h, S, crew, pass, lim, wc);
    };
    float perks = 0, flaws = 0;
    for (int b = 0; b < 6; b++)
        if (P >> b & 1) perks += pc(b + 1);
    if (P >> 6 & 3) perks += 2.f * pc(7, float(P >> 6 & 3));
    if (P >> 8 & 1) perks += pc(8);
    if (P >> 9 & 1) perks += pc(9);
    if (P >> 10 & 7) perks += pc(10, float(P >> 10 & 7));
    if (P >> 13 & 7) perks += pc(11, float(P >> 13 & 7));
    if (P >> 16 & 1) perks += pc(12, 0, 0, 0, 1, 0);
    if (P >> 17 & 1) perks += pc(14);
    if (P >> 18 & 15) perks += pc(15, float(P >> 18 & 15));
    if (P >> 22 & 3) perks += 2.f;
    if (P >> 24 & 3) perks += pc(21, 0, float(P >> 24 & 3), v.size);
    if (P >> 26 & 1) perks += pc(22);
    if (P >> 27 & 3) perks += pc(23, float(P >> 27 & 3), 0, v.size);
    for (int k = 0; k < 4; k++)
        if (F >> (2 * k) & 3) perks += pc(24, float((F >> (2 * k) & 3) << 1));
    if (F >> 8 & 1) perks += pc(25);
    if (F >> 9 & 1) perks += pc(26);
    if (P >> 30 & 1) perks += pc(27);
    if (P >> 31 & 1) perks += pc(29);
    if (P >> 29 & 1) {  // sniper systems: weapons other than melee / grenades
        static const int skip[] = {0x12, 0x16, 0x22, 0x4a, 7, 0x4d, 0x58, 0x59, 0x4c};
        int n = wcount;
        for (auto& w : v.weapons)
            for (int s : skip)
                if (w.tableId == s) n--;
        perks += pc(30, 0, 0, 0, 0, 0, false, n);
    }
    if (F >> 10 & 7) perks += pc(31, float(F >> 10 & 7));
    if (F >> 13 & 1) perks += pc(32, 0, 11);
    if (F >> 14 & 1) perks += pc(34);
    if (F >> 15 & 3) flaws += pc(35, float((F >> 15 & 3) << 1));
    if (F >> 17 & 3) flaws += pc(36, float((F >> 17 & 3) << 1));
    if (F >> 19 & 1) flaws += pc(37);
    if (F >> 20 & 1) flaws += pc(39);
    if (F >> 21 & 1) flaws += pc(38);
    if (F >> 22 & 1) flaws += pc(40);
    if (F >> 23 & 1) flaws += pc(42);
    if (F >> 24 & 1) flaws += pc(43);
    if (F >> 25 & 1) flaws += pc(45, float(F >> 25 & 1));
    if (F >> 26 & 1) flaws += pc(49);
    if (F >> 27 & 1) flaws += pc(50);
    for (int k = 28; k < 32; k++)
        if (F >> k & 1) flaws += pc(51, float(v.armor));
    double pf = std::max(0.0, double(perks - flaws));
    double t = double(v.sensors) + 1.0 + pf;
    if (t < 0) t = 0;
    double misc = 103.0 + t * t;
    auto up = [](double x) { int i = int(x); return double(i) < x ? i + 1 : i; };  // FUN_100fcba0
    Threat r;
    r.offensive = up(off), r.defensive = up(def), r.misc = up(misc);
    r.total = up((std::max(1.0, off) + std::max(1.0, def) + std::max(1.0, misc)) / 3.0);
    return r;
}

int GameData::weaponIndex(const std::string& wpn) const {
    std::string u = upper(wpn);
    for (size_t i = 0; i < weaponFiles.size(); i++)
        if (weaponFiles[i] == u) return int(i);
    return -1;
}

std::string GameData::text(const std::string& file) const {
    Bytes b;
    if (!fs->read(file, b)) return std::string();
    return std::string(b.begin(), b.end());
}

bool GameData::hasMovie(const std::string& name) const { return !name.empty() && !fs->findLoose("shell/movies/" + name).empty(); }

}  // namespace hg2
