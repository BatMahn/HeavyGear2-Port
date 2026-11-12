// The game shell (shell.dll): main menu, options, the single-player campaign (pilot registration, briefings,
// squad and gear selection, debriefings, autosaves), historical and training missions, and instant action.
// Screens are composed from the LDL layout parts and drawn on the original artwork.
#pragma once
#include <string>
#include <vector>

#include "../app.h"
#include "../game/session.h"
#include "gamedata.h"
#include "movie.h"
#include "../net/net.h"
#include <memory>
#include <map>
#include "ui.h"

namespace hg2 {

class Shell {
public:
    explicit Shell(AppContext& app) : app_(app) {}
    bool init();
    void run();
    // Offscreen test: show `screen` (main, single, options, sound, graphics, controls, credits, pilots, load,
    // missions:training|historical, briefing:<msn>, squad:<msn>, gear:<msn>, debrief:<msn>:<outcome>, instant)
    // with the mouse at (mx, my) in 640x480 units and save a PNG.
    bool screenshot(const std::string& screen, const std::string& png, float mx, float my);
    void setMovies(bool on) { moviesEnabled_ = on; }

private:
    enum class Scr { Main, Single, Options, Sound, Graphics, Controls, Credits, Pilots, Load, Missions, Briefing, Squad, Gear, Debrief, Instant, Multi };
    enum class Mode { Campaign, Historical, Training, Instant, Multi };
    struct Slot {                    // a gear in the squad: pilot, chassis and variant
        std::string pilot;
        int chassis = -1;            // GameData::chassis index; -1 = the mission's own gear
        int variant = 0;
        bool used = true;
        bool custom = false;         // changed in the shell (else historical / training keep the mission's own gear)
        bool refit = false;          // weapons changed in the gear lab: `weapons` replaces the variant's loadout
        std::vector<std::pair<int, int>> weapons;  // hardpoint, weapon index
        bool restat = false;         // structure / perks / flaws changed in the gear lab: `rec` replaces the variant's
        VarRecord rec;
    };
    struct Save { std::string mission; char kind = 'L'; std::string date; std::vector<std::string> lost; };
    struct CampaignPilot {
        std::string name, chassis, variant = "Standard", mission = "S2J";
        std::string refit;           // gear lab weapons "slot:WEAPON.WPN,..." (empty: the variant's own)
        std::string restat;          // gear lab structure "armor,sensors,fc,maneuver,perks hex,flaws hex" (empty: own)
        std::vector<std::string> lost, squad;  // squad mates killed in action; last squad picked
        int kills = 0, sorties = 0, deaths = 0, shots = 0, hits = 0;
        float difficulty = 0.5f;
        std::vector<Save> saves;
    };

    void frame();
    void go(Scr s);
    void background(const std::string& tga);
    void title(const std::string& t, float y = 18);
    // screens
    void mainMenu();
    void singleMenu();
    void options();
    void soundOptions();
    void graphicsOptions();
    void controls();
    void credits();
    void pilots();
    void loadGame();
    void missionList();
    void briefing();
    void squad();
    void gear();
    void debrief();
    void instantAction();
    void multiplayer();
    // flow
    bool openMission(const std::string& id);   // loads msn_ and the default squad
    void defaultSquad();
    void launch();
    std::vector<std::pair<int, int>> loadoutFor(const Slot& s) const;
    std::vector<int> weaponsForHardpoint(int slot);  // weapons stock variants mount on that hardpoint
    std::map<int, std::vector<int>> hardpointWeapons_;
    std::vector<int> refitChoices_;
    // threat values (FUN_100bf7bf) of a squad slot; false for the mission's own gear
    bool slotThreat(const Slot& s, Threat& out) const;
    GearChoice gearChoice(const Slot& s) const;
    bool slotRecord(const Slot& s, VarRecord& out) const;  // the variant's record with gear-lab changes
    std::string restatString(const Slot& s) const;
    void applyRestat(Slot& s, const std::string& r) const;
    std::vector<int> perkChoices_;   // perk popup rows -> GameData::perkFields() index
    std::string threatDlg_;          // open "exceeds threat" dialog (layout window name)
    std::string refitString(const Slot& s) const;
    void applyRefit(Slot& s, const std::string& r) const;
    std::string slotChassisName(const Slot& s) const;
    std::string slotVariantName(const Slot& s) const;
    int defaultVariant(int chassis) const;
    int missionGear(const std::string& missionId) const;  // chassis index of the mission's own user gear
    void playBriefingAudio();
    // movies: full-screen (title, campaign cut-scenes; any key or click skips) or in the briefing's video box
    std::string findMovie(const std::string& name, bool shellMovie) const;
    bool startMovie(const std::string& name, bool shellMovie, bool fullscreen);
    void stopMovie();
    bool updateMovie();               // advances the playing movie; false when it ended
    bool fullscreenMovie();           // draws a full-screen movie (and handles skipping); false when none
    void queueMovie(const std::string& name) { if (!name.empty()) movieQueue_.push_back(name); }
    std::string missionName(const MsnInfo& m) const;
    // pull-down list (popup drawn over everything); returns the picked index or -1
    int dropdown(const std::string& path, const std::vector<std::string>& items, int sel, const std::string& id);
    void drawPopup();
    bool slider(const UiRect& r, float& value);  // 0..1 horizontal scroller (HORIZSCROLLBAR art)
    // persistence
    void loadRoster();
    void saveRoster();
    void autosave(char kind);
    CampaignPilot* pilot() { return cur_ >= 0 && size_t(cur_) < roster_.size() ? &roster_[size_t(cur_)] : nullptr; }

    AppContext& app_;
    Ui ui_;
    GameData data_;
    Scr scr_ = Scr::Main, back_ = Scr::Main;
    Mode mode_ = Mode::Campaign;
    bool quit_ = false;
    // campaign
    std::vector<CampaignPilot> roster_;
    int cur_ = -1;
    bool registering_ = false;
    std::string newName_;
    int newChassis_ = 0;
    int pilotScroll_ = 0, notesScroll_ = 0, loadSel_ = 0, loadScroll_ = 0;
    bool confirmDelete_ = false;
    // mission in preparation
    MsnInfo msn_;
    Slot user_;
    std::vector<Slot> squad_;        // chosen squad mates
    std::vector<std::string> missions_;  // historical / training list
    int listSel_ = 0, listScroll_ = 0, briefScroll_ = 0, bioScroll_ = 0;
    std::string bioPilot_;
    int gearSlot_ = 0;               // gear screen: 0 = user, 1.. squad, 100+ instant-action enemies
    int gearNotesScroll_ = 0;
    // instant action
    std::vector<Slot> iaSquad_, iaEnemies_;
    int iaMap_ = 0, iaWaves_ = 3;
    // results
    MissionResult result_;
    bool haveResult_ = false;
    // popup list state
    struct Popup { std::string id; UiRect anchor; std::vector<std::string> items; int sel = -1, scroll = 0; bool open = false; int picked = -1; };
    Popup popup_;
    std::string dragging_;
    int creditScroll_ = 0;
    float creditT_ = 0;
    Uint64 lastTick_ = 0;
    bool humOn_ = false;
    struct Playback {
        std::unique_ptr<Movie> m;
        Image img;
        int tex = -1, shown = -1;
        Uint64 t0 = 0;
        bool fullscreen = false;
    };
    Playback movie_;
    std::vector<std::string> movieQueue_;
    std::map<std::pair<int, int>, int> movieTex_;  // renderer texture per frame size
    bool moviesEnabled_ = true;
    // network play
    NetGame net_;
    std::string mpAddr_ = "127.0.0.1", mpName_;
    int mpField_ = 0;              // focused text field: 0 name, 1 address
    int mpArena_ = 0, mpFrags_ = 10, mpMinutes_ = 10;
    NetGear mpGear() const;
    std::string mpScores_;
};

}  // namespace hg2
