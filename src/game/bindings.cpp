#include "bindings.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hg2 {
namespace {

const CmdInfo kCmds[] = {
    {"Forward", 0, true}, {"Backward", 0, true}, {"Steer Left", 0, true}, {"Steer Right", 0, true},
    {"Increase Throttle", 0, false}, {"Decrease Throttle", 0, false}, {"Reverse Direction", 0, false},
    {"1/3 Throttle", 0, false}, {"2/3 Throttle", 0, false}, {"Full Throttle", 0, false}, {"Full Stop", 0, false},
    {"Jump Jets", 0, false}, {"Jump", 0, false}, {"Crouch", 0, false}, {"Stand Up", 0, false}, {"Wheels", 0, false},
    {"Side Step Left", 0, true}, {"Side Step Right", 0, true}, {"Roll Left", 0, true}, {"Roll Right", 0, true},
    {"Center Torso", 0, false},
    {"Weapon Fire", 1, false}, {"Weapon Cycle", 1, false}, {"Target ReticleX", 1, true}, {"Target ReticleY", 1, true},
    {"Change Weapon", 1, false}, {"Pick Up Weapon", 1, false}, {"Change Grenade", 1, false}, {"Throw Grenade", 1, false},
    {"Fire Artillery Strike", 1, false}, {"Target Cycle", 1, false}, {"Previous Target", 1, false},
    {"Nearest Enemy", 1, false}, {"Target Friendly", 1, false}, {"Target At Reticle", 1, false},
    {"Laser Targeting", 1, false}, {"Radar Zoom", 1, false}, {"Active Radar Config", 1, false},
    {"Imaging System", 1, false}, {"HUD Display ON-OFF", 1, false}, {"HUD Color", 1, false}, {"ECM Enable", 1, false},
    {"Tactical Map", 2, false}, {"Stats Display Toggle", 2, false}, {"General Chat", 2, false}, {"Team Chat", 2, false},
    {"User Taunt 1", 2, false}, {"User Taunt 2", 2, false}, {"User Taunt 3", 2, false}, {"User Taunt 4", 2, false},
    {"User Taunt 5", 2, false}, {"User Taunt 6", 2, false}, {"User Taunt 7", 2, false}, {"User Taunt 8", 2, false},
    {"Command Group", 2, false}, {"Squad Fire At Will", 2, false}, {"Squad Regroup", 2, false},
    {"Squad Hold Ground", 2, false}, {"Squad Defend My Target", 2, false}, {"Squad Attack My Target", 2, false},
    {"Squad Goto Checkpoint", 2, false},
    {"Goals", 3, false}, {"Select Nav Point", 3, false}, {"Console Display Toggle", 3, false}, {"Zoom In", 3, false},
    {"Zoom Out", 3, false}, {"External Tracking", 3, false}, {"Camera Left", 3, false}, {"Camera Right", 3, false},
    {"Camera Up", 3, false}, {"Camera Down", 3, false}, {"Abort", 3, false},
};
static_assert(sizeof(kCmds) / sizeof(kCmds[0]) == size_t(Cmd::Count), "command table");

// key names as the bindings files spell them
struct KeyName { const char* name; SDL_Scancode sc; };
const KeyName kKeys[] = {
    {"Numpad 0", SDL_SCANCODE_KP_0}, {"Numpad 1", SDL_SCANCODE_KP_1}, {"Numpad 2", SDL_SCANCODE_KP_2},
    {"Numpad 3", SDL_SCANCODE_KP_3}, {"Numpad 4", SDL_SCANCODE_KP_4}, {"Numpad 5", SDL_SCANCODE_KP_5},
    {"Numpad 6", SDL_SCANCODE_KP_6}, {"Numpad 7", SDL_SCANCODE_KP_7}, {"Numpad 8", SDL_SCANCODE_KP_8},
    {"Numpad 9", SDL_SCANCODE_KP_9}, {"Numpad +", SDL_SCANCODE_KP_PLUS}, {"Numpad -", SDL_SCANCODE_KP_MINUS},
    {"Numpad .", SDL_SCANCODE_KP_PERIOD}, {"Numpad Enter", SDL_SCANCODE_KP_ENTER}, {"Numpad /", SDL_SCANCODE_KP_DIVIDE},
    {"Numpad *", SDL_SCANCODE_KP_MULTIPLY}, {"Backspace", SDL_SCANCODE_BACKSPACE}, {"[", SDL_SCANCODE_LEFTBRACKET},
    {"]", SDL_SCANCODE_RIGHTBRACKET}, {"\\", SDL_SCANCODE_BACKSLASH}, {"=", SDL_SCANCODE_EQUALS},
    {"-", SDL_SCANCODE_MINUS}, {"/", SDL_SCANCODE_SLASH}, {"`", SDL_SCANCODE_GRAVE}, {";", SDL_SCANCODE_SEMICOLON},
    {"'", SDL_SCANCODE_APOSTROPHE}, {",", SDL_SCANCODE_COMMA}, {".", SDL_SCANCODE_PERIOD},
    {"Tab", SDL_SCANCODE_TAB}, {"Escape", SDL_SCANCODE_ESCAPE}, {"Enter", SDL_SCANCODE_RETURN},
    {"Space", SDL_SCANCODE_SPACE}, {"Home", SDL_SCANCODE_HOME}, {"End", SDL_SCANCODE_END},
    {"Insert", SDL_SCANCODE_INSERT}, {"Delete", SDL_SCANCODE_DELETE}, {"Page Up", SDL_SCANCODE_PAGEUP},
    {"Page Down", SDL_SCANCODE_PAGEDOWN}, {"Left Arrow", SDL_SCANCODE_LEFT}, {"Right Arrow", SDL_SCANCODE_RIGHT},
    {"Up Arrow", SDL_SCANCODE_UP}, {"Down Arrow", SDL_SCANCODE_DOWN}, {"Right Shift", SDL_SCANCODE_RSHIFT},
    {"Left Shift", SDL_SCANCODE_LSHIFT}, {"Right Ctrl", SDL_SCANCODE_RCTRL}, {"Left Ctrl", SDL_SCANCODE_LCTRL},
    {"Right Alt", SDL_SCANCODE_RALT}, {"Left Alt", SDL_SCANCODE_LALT}, {"Caps Lock", SDL_SCANCODE_CAPSLOCK},
    {"F1", SDL_SCANCODE_F1}, {"F2", SDL_SCANCODE_F2}, {"F3", SDL_SCANCODE_F3}, {"F4", SDL_SCANCODE_F4},
    {"F5", SDL_SCANCODE_F5}, {"F6", SDL_SCANCODE_F6}, {"F7", SDL_SCANCODE_F7}, {"F8", SDL_SCANCODE_F8},
    {"F9", SDL_SCANCODE_F9}, {"F10", SDL_SCANCODE_F10}, {"F11", SDL_SCANCODE_F11}, {"F12", SDL_SCANCODE_F12},
};

bool iequals(const std::string& a, const char* b) {
    size_t n = strlen(b);
    if (a.size() != n) return false;
    for (size_t i = 0; i < n; i++)
        if (tolower(static_cast<unsigned char>(a[i])) != tolower(static_cast<unsigned char>(b[i]))) return false;
    return true;
}

// The shell's Reset Default set (Shell\Bindings\Defaults.dft): command, control, link
struct Def { Cmd cmd; const char* control; int link; };
const Def kDefaults[] = {
    {Cmd::Forward, "Numpad 8", 0}, {Cmd::Backward, "Numpad 2", 0}, {Cmd::SteerLeft, "Numpad 4", 0},
    {Cmd::SteerRight, "Numpad 6", 0}, {Cmd::IncreaseThrottle, "Numpad +", 0}, {Cmd::DecreaseThrottle, "Numpad -", 0},
    {Cmd::ReverseDirection, "Backspace", 0}, {Cmd::Throttle13, "[", 0}, {Cmd::Throttle23, "]", 0},
    {Cmd::FullThrottle, "\\", 0}, {Cmd::FullStop, "=", 0}, {Cmd::JumpJets, "Numpad .", 0},
    {Cmd::Jump, "Numpad Enter", 0}, {Cmd::Crouch, "Numpad /", 0}, {Cmd::StandUp, "Numpad *", 0},
    {Cmd::Wheels, "Numpad 0", 0}, {Cmd::SideStepLeft, "Numpad 7", 0}, {Cmd::SideStepRight, "Numpad 9", 0},
    {Cmd::RollLeft, "Numpad 1", 0}, {Cmd::RollRight, "Numpad 3", 0}, {Cmd::CenterTorso, "Numpad 5", 0},
    {Cmd::WeaponFire, "Mouse Button 0", 0}, {Cmd::WeaponCycle, "Mouse Button 1", 0},
    {Cmd::ReticleX, "Mouse X-Axis", 0}, {Cmd::ReticleY, "Mouse Y-Axis", 0}, {Cmd::ChangeWeapon, "Right Shift", 0},
    {Cmd::PickUpWeapon, "/", 0}, {Cmd::ChangeGrenade, "B", 0}, {Cmd::ThrowGrenade, "G", 0},
    {Cmd::FireArtilleryStrike, "S", 0}, {Cmd::TargetCycle, "T", 0}, {Cmd::PreviousTarget, "Y", 0},
    {Cmd::NearestEnemy, "E", 0}, {Cmd::TargetFriendly, "F", 0}, {Cmd::TargetAtReticle, "R", 0},
    {Cmd::LaserTargeting, "L", 0}, {Cmd::RadarZoom, "Z", 0}, {Cmd::ActiveRadarConfig, "A", 0},
    {Cmd::ImagingSystem, "I", 0}, {Cmd::HudDisplay, "H", 1}, {Cmd::HudColor, "H", 2}, {Cmd::EcmEnable, "E", 1},
    {Cmd::TacticalMap, "Tab", 0}, {Cmd::StatsDisplay, "F1", 0}, {Cmd::GeneralChat, "F2", 0}, {Cmd::TeamChat, "F3", 0},
    {Cmd::Taunt1, "F5", 0}, {Cmd::Taunt2, "F6", 0}, {Cmd::Taunt3, "F7", 0}, {Cmd::Taunt4, "F8", 0},
    {Cmd::Taunt5, "F9", 0}, {Cmd::Taunt6, "F10", 0}, {Cmd::Taunt7, "F11", 0}, {Cmd::Taunt8, "F12", 0},
    {Cmd::CommandGroup, "C", 0}, {Cmd::SquadFireAtWill, "A", 1}, {Cmd::SquadRegroup, "R", 1},
    {Cmd::SquadHoldGround, "S", 1}, {Cmd::SquadDefendMyTarget, "D", 1}, {Cmd::SquadAttackMyTarget, "T", 1},
    {Cmd::SquadGotoCheckpoint, "G", 1},
    {Cmd::Goals, "O", 0}, {Cmd::SelectNavPoint, "N", 0}, {Cmd::ConsoleDisplay, "`", 0}, {Cmd::ZoomIn, "Home", 0},
    {Cmd::ZoomOut, "End", 0}, {Cmd::ExternalTracking, "C", 1}, {Cmd::CameraLeft, "Left Arrow", 0},
    {Cmd::CameraRight, "Right Arrow", 0}, {Cmd::CameraUp, "Up Arrow", 0}, {Cmd::CameraDown, "Down Arrow", 0},
    {Cmd::Abort, "Escape", 0},
};

int sdlButton(int di) { return di == 0 ? SDL_BUTTON_LEFT : di == 1 ? SDL_BUTTON_RIGHT : di == 2 ? SDL_BUTTON_MIDDLE : di == 3 ? SDL_BUTTON_X1 : SDL_BUTTON_X2; }

bool modHeld(int link, SDL_Keymod mod) {
    return link == 1 ? (mod & SDL_KMOD_CTRL) != 0 : link == 2 ? (mod & SDL_KMOD_ALT) != 0 : link == 3 ? (mod & SDL_KMOD_SHIFT) != 0 : true;
}

}  // namespace

const CmdInfo& Bindings::info(Cmd c) { return kCmds[size_t(c)]; }

const char* Bindings::categoryName(int category) {
    static const char* n[] = {"MOVEMENT", "TARGETING_HUD", "SQUAD_NETWORK", "MISCELLANEOUS"};
    return n[category & 3];
}

void Bindings::setDefaults() {
    list.clear(), loaded_ = false;
    for (const Def& d : kDefaults) {
        Binding b;
        b.cmd = d.cmd, b.link = d.link;
        if (parseControl(d.control, b.kind, b.code)) list.push_back(b);
    }
}

void Bindings::clear(Cmd c) {
    for (size_t i = list.size(); i-- > 0;)
        if (list[i].cmd == c) list.erase(list.begin() + long(i));
}

std::string Bindings::controlName(Binding::Kind kind, int code) {
    if (kind == Binding::MouseButton) return "Mouse Button " + std::to_string(code);
    if (kind == Binding::MouseAxis) return code == 0 ? "Mouse X-Axis" : "Mouse Y-Axis";
    for (const KeyName& k : kKeys)
        if (k.sc == code) return k.name;
    SDL_Scancode sc = SDL_Scancode(code);
    if (sc >= SDL_SCANCODE_A && sc <= SDL_SCANCODE_Z) return std::string(1, char('A' + (sc - SDL_SCANCODE_A)));
    if (sc >= SDL_SCANCODE_1 && sc <= SDL_SCANCODE_9) return std::string(1, char('1' + (sc - SDL_SCANCODE_1)));
    if (sc == SDL_SCANCODE_0) return "0";
    const char* n = SDL_GetScancodeName(sc);
    return n && *n ? n : "?";
}

bool Bindings::parseControl(const std::string& name, Binding::Kind& kind, int& code) {
    if (name.rfind("Mouse Button ", 0) == 0) return kind = Binding::MouseButton, code = atoi(name.c_str() + 13), true;
    if (iequals(name, "Mouse X-Axis")) return kind = Binding::MouseAxis, code = 0, true;
    if (iequals(name, "Mouse Y-Axis")) return kind = Binding::MouseAxis, code = 1, true;
    kind = Binding::Key;
    for (const KeyName& k : kKeys)
        if (iequals(name, k.name)) return code = k.sc, true;
    if (name.size() == 1) {
        char c = char(toupper(static_cast<unsigned char>(name[0])));
        if (c >= 'A' && c <= 'Z') return code = SDL_SCANCODE_A + (c - 'A'), true;
        if (c >= '1' && c <= '9') return code = SDL_SCANCODE_1 + (c - '1'), true;
        if (c == '0') return code = SDL_SCANCODE_0, true;
    }
    SDL_Scancode sc = SDL_GetScancodeFromName(name.c_str());
    if (sc == SDL_SCANCODE_UNKNOWN) return false;
    code = sc;
    return true;
}

const char* Bindings::linkName(int link) { return link == 1 ? "Ctrl" : link == 2 ? "Alt" : link == 3 ? "Shift" : ""; }

std::string Bindings::describe(const Binding& b) const {
    std::string s = controlName(b.kind, b.code);
    if (b.link) s = std::string(linkName(b.link)) + "+" + s;
    return s;
}

std::string Bindings::describe(Cmd c) const {
    std::string s;
    for (const Binding& b : list)
        if (b.cmd == c) s += (s.empty() ? "" : ", ") + describe(b);
    return s;
}

std::vector<std::string> Bindings::serialize() const {
    std::vector<std::string> out;
    for (const Binding& b : list) {
        char sens[32];
        snprintf(sens, sizeof sens, "%.3f", b.sens);
        out.push_back(std::string(info(b.cmd).name) + "|" + controlName(b.kind, b.code) + "|" + linkName(b.link) + "|" + sens +
                      (b.reverse ? "|reverse" : ""));
    }
    return out;
}

bool Bindings::loadLine(const std::string& value) {
    std::vector<std::string> f;
    size_t a = 0;
    for (;;) {
        size_t e = value.find('|', a);
        f.push_back(value.substr(a, e == std::string::npos ? std::string::npos : e - a));
        if (e == std::string::npos) break;
        a = e + 1;
    }
    if (f.size() < 2) return false;
    Binding b;
    for (int i = 0; i < int(Cmd::Count); i++)
        if (iequals(f[0], kCmds[i].name)) b.cmd = Cmd(i);
    if (b.cmd == Cmd::Count || !parseControl(f[1], b.kind, b.code)) return false;
    if (f.size() > 2) b.link = iequals(f[2], "Ctrl") ? 1 : iequals(f[2], "Alt") ? 2 : iequals(f[2], "Shift") ? 3 : 0;
    if (f.size() > 3 && !f[3].empty()) b.sens = float(atof(f[3].c_str()));
    if (f.size() > 4) b.reverse = iequals(f[4], "reverse");
    list.push_back(b);
    return true;
}

const Binding* Bindings::first(Cmd c) const {
    for (const Binding& b : list)
        if (b.cmd == c) return &b;
    return nullptr;
}

bool Bindings::conflicts(const Binding& b) const {
    for (const Binding& o : list)
        if (&o != &b && o.kind == b.kind && o.code == b.code && o.link == b.link) return true;
    return false;
}

bool Bindings::linkOk(const Binding& b, SDL_Keymod mod) const {
    if (b.link) return modHeld(b.link, mod);
    // an unlinked binding gives way while a modifier is held that another binding of the same control is linked to
    // (E is Nearest Enemy, Ctrl+E ECM Enable)
    for (const Binding& o : list)
        if (o.link && o.kind == b.kind && o.code == b.code && modHeld(o.link, mod)) return false;
    return true;
}

bool Bindings::held(Cmd c, const bool* keys, SDL_MouseButtonFlags buttons) const {
    SDL_Keymod mod = SDL_GetModState();
    for (const Binding& b : list) {
        if (b.cmd != c || !linkOk(b, mod)) continue;
        if (b.kind == Binding::Key && keys && b.code > 0 && b.code < SDL_SCANCODE_COUNT && keys[b.code]) return true;
        if (b.kind == Binding::MouseButton && (buttons & SDL_BUTTON_MASK(sdlButton(b.code)))) return true;
    }
    return false;
}

bool Bindings::pressed(Cmd c, const SDL_Event& ev) const {
    for (const Binding& b : list) {
        if (b.cmd != c) continue;
        if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat && b.kind == Binding::Key && int(ev.key.scancode) == b.code &&
            linkOk(b, ev.key.mod))
            return true;
        if (ev.type == SDL_EVENT_MOUSE_BUTTON_DOWN && b.kind == Binding::MouseButton && ev.button.button == sdlButton(b.code) &&
            linkOk(b, SDL_GetModState()))
            return true;
    }
    return false;
}

float Bindings::axisScale(Cmd c, int axis) const {
    for (const Binding& b : list)
        if (b.cmd == c && b.kind == Binding::MouseAxis && b.code == axis) return b.reverse ? -b.sens : b.sens;
    return 0.f;
}

bool Bindings::usesKey(SDL_Scancode sc) const {
    for (const Binding& b : list)
        if (b.kind == Binding::Key && b.code == int(sc)) return true;
    return false;
}

}  // namespace hg2
