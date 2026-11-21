// The user's control bindings: the sim's named commands (hg2sim FUN_10132070 registers them with the input manager
// FUN_10135260) and the controls they are bound to. The defaults are the shell's Reset Default set
// (Shell\Bindings\Defaults.dft, the same layout bindings.def ships with plus the squad commands); the shell's
// controls screen (BINDINGS.LDL bindings_config_window) changes them and the port keeps them in settings.cfg.
//
// A binding is a control (a key, a mouse button or a mouse axis), an optional LINK modifier (Ctrl / Alt / Shift, the
// SYSTEM block of the file) and a sensitivity (0..1, 0.5 by default; it scales the mouse axes).
#pragma once
#include <SDL3/SDL.h>

#include <string>
#include <vector>

namespace hg2 {

enum class Cmd {
    // MOVEMENT
    Forward, Backward, SteerLeft, SteerRight, IncreaseThrottle, DecreaseThrottle, ReverseDirection, Throttle13,
    Throttle23, FullThrottle, FullStop, JumpJets, Jump, Crouch, StandUp, Wheels, SideStepLeft, SideStepRight, RollLeft,
    RollRight, CenterTorso,
    // TARGETING_HUD
    WeaponFire, WeaponCycle, ReticleX, ReticleY, ChangeWeapon, PickUpWeapon, ChangeGrenade, ThrowGrenade,
    FireArtilleryStrike, TargetCycle, PreviousTarget, NearestEnemy, TargetFriendly, TargetAtReticle, LaserTargeting,
    RadarZoom, ActiveRadarConfig, ImagingSystem, HudDisplay, HudColor, EcmEnable,
    // SQUAD_NETWORK
    TacticalMap, StatsDisplay, GeneralChat, TeamChat, Taunt1, Taunt2, Taunt3, Taunt4, Taunt5, Taunt6, Taunt7, Taunt8,
    CommandGroup, SquadFireAtWill, SquadRegroup, SquadHoldGround, SquadDefendMyTarget, SquadAttackMyTarget,
    SquadGotoCheckpoint,
    // MISCELLANEOUS
    Goals, SelectNavPoint, ConsoleDisplay, ZoomIn, ZoomOut, ExternalTracking, CameraLeft, CameraRight, CameraUp,
    CameraDown, Abort,
    Count
};

struct CmdInfo {
    const char* name;   // the sim's command name (also the bindings file's)
    int category;       // 0 movement, 1 targeting / HUD, 2 squad / network, 3 miscellaneous
    bool axis;          // registered as an axis (FUN_10135b90 / type 3): takes a sensitivity
};

struct Binding {
    enum Kind { Key, MouseButton, MouseAxis };
    Cmd cmd = Cmd::Count;
    Kind kind = Key;
    int code = 0;        // SDL_Scancode, DirectInput mouse button (0 left, 1 right, 2 middle, 3/4 extra) or axis (0 X, 1 Y)
    int link = 0;        // 0 none, 1 Ctrl, 2 Alt, 3 Shift
    float sens = 0.5f;
    bool reverse = false;  // the controls screen's Reversed switch (binding +0x54: the value's sign flips)
};

class Bindings {
public:
    Bindings() { setDefaults(); }
    std::vector<Binding> list;

    static const CmdInfo& info(Cmd c);
    static const char* categoryName(int category);
    void setDefaults();
    void clear(Cmd c);
    // the bindings file's control names ("Numpad 8", "Mouse Button 0", "Mouse X-Axis", "Left Arrow", "E")
    static std::string controlName(Binding::Kind kind, int code);
    static bool parseControl(const std::string& name, Binding::Kind& kind, int& code);
    static const char* linkName(int link);
    // "Ctrl+E" style text of a command's bindings for the shell and the training prompts
    std::string describe(Cmd c) const;
    std::string describe(const Binding& b) const;
    // settings.cfg lines: "bind=<command>|<control>|<link>|<sensitivity>"
    std::vector<std::string> serialize() const;
    void beginLoad() { list.clear(), loaded_ = true; }
    bool loadLine(const std::string& value);
    bool loaded() const { return loaded_; }
    // the first binding of a command (for sensitivity), or null
    const Binding* first(Cmd c) const;
    // the bindings that share a control (the conflict alert)
    bool conflicts(const Binding& b) const;

    // Runtime. held: the command's key or mouse button is down (keyboard state, SDL mouse button mask). pressed: this
    // key / mouse-button event triggers the command.
    bool held(Cmd c, const bool* keys, SDL_MouseButtonFlags buttons) const;
    bool pressed(Cmd c, const SDL_Event& ev) const;
    // a mouse axis command's sensitivity (negative when reversed), 0 when the axis is not bound
    float axisScale(Cmd c, int axis) const;
    // true when some binding uses this key (the port's fixed extras give way to the user's bindings)
    bool usesKey(SDL_Scancode sc) const;

private:
    bool linkOk(const Binding& b, SDL_Keymod mod) const;
    bool loaded_ = false;
};

}  // namespace hg2
