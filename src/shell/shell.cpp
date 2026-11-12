#include "shell.h"

#include <SDL3/SDL.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <set>
#include <sstream>

#include "../assets/gear.h"
#include "../assets/mission.h"
#include "avi.h"

namespace hg2 {

namespace {
const char* kSplashArt = "SBksplsh.TGA";   // hg2_asset_strings string0..4
const char* kCharArt = "Sbkchrtr.TGA";
const char* kTrideoArt = "Sbackgnd.TGA";
const char* kTacticalArt = "SBAKTACT.TGA";

std::vector<std::string> split(const std::string& s, char sep) {
    std::vector<std::string> out;
    std::string cur;
    for (char c : s) {
        if (c == sep) {
            if (!cur.empty()) out.push_back(cur);
            cur.clear();
        } else {
            cur += c;
        }
    }
    if (!cur.empty()) out.push_back(cur);
    return out;
}
std::string join(const std::vector<std::string>& v, char sep) {
    std::string s;
    for (auto& x : v) s += (s.empty() ? "" : std::string(1, sep)) + x;
    return s;
}
std::string trimNl(std::string s) {
    while (!s.empty() && (s.back() == '\n' || s.back() == '\r' || s.back() == ' ')) s.pop_back();
    return s;
}
std::string nowString() {
    time_t t = time(nullptr);
    char buf[32];
    strftime(buf, sizeof buf, "%Y-%m-%d %H:%M", localtime(&t));
    return buf;
}
std::string hhmmss(float secs) {
    int s = int(secs);
    char b[16];
    snprintf(b, sizeof b, "%02d:%02d:%02d", s / 3600, (s / 60) % 60, s % 60);
    return b;
}
// the campaign's mission order (S2J "Defection" opens; MSN next ids continue it)
const char* kCampaignStart = "S2J";
}  // namespace

// ---------------------------------------------------------------- settings
bool Settings::load(const std::string& path) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    while (std::getline(f, line)) {
        size_t eq = line.find('=');
        if (eq == std::string::npos) continue;
        std::string k = line.substr(0, eq);
        float v = float(atof(line.c_str() + eq + 1));
        if (k == "effects") effects = v;
        else if (k == "dialog") dialog = v;
        else if (k == "music") music = v;
        else if (k == "shell") shell = v;
        else if (k == "external") externalView = v != 0;
        else if (k == "invert") invertMouse = v != 0;
        else if (k == "mouse") mouseSpeed = std::clamp(v, 0.25f, 3.f);
        else if (k == "particles") particles = std::clamp(int(v), 0, 2);
        else if (k == "fullscreen") fullscreen = v != 0;
        else if (k == "msaa") msaa = int(v) >= 8 ? 8 : int(v) >= 4 ? 4 : int(v) >= 2 ? 2 : 1;
        else if (k == "moviefilter") movieFilter = std::clamp(int(v), 0, 2);
        else if (k == "movie43") movie43 = v != 0;
        else if (k == "data") dataDir = line.substr(eq + 1);
    }
    return true;
}

bool Settings::save(const std::string& path) const {
    std::ofstream f(path);
    if (!f) return false;
    f << "effects=" << effects << "\ndialog=" << dialog << "\nmusic=" << music << "\nshell=" << shell << "\nexternal=" << externalView
      << "\ninvert=" << invertMouse << "\nmouse=" << mouseSpeed << "\nparticles=" << particles << "\nfullscreen=" << fullscreen
      << "\nmsaa=" << msaa << "\nmoviefilter=" << movieFilter << "\nmovie43=" << movie43 << "\n";
    if (!dataDir.empty()) f << "data=" << dataDir << "\n";
    return true;
}

// ---------------------------------------------------------------- setup
bool Shell::init() {
    if (!data_.load(app_.fs)) SDL_Log("shell: GRINVTRY.TXT missing (hg2shell.prj)");
    if (!ui_.init(app_.r, app_.fs, app_.audioOk ? &app_.audio : nullptr)) {
        SDL_Log("shell: layouts or artwork missing; the shell needs hg2shell.prj and the shell art (rim565.zfs or zfs.zip)");
        return false;
    }
    loadRoster();
    if (getenv("HG2_THREAT"))
        for (auto& ch : data_.chassis)
            for (auto& v : ch.variants) {
                VarRecord r;
                if (!data_.variantRecord(v.file, r)) continue;
                Threat t = data_.threat(r);
                SDL_Log("THREAT %-16s %-14s %5d  off %5d def %4d misc %4d  (sens %d fc %d arm %d size %d man %d mp %d/%d perks %08x flaws %08x w %zu)",
                        ch.name.c_str(), v.name.c_str(), t.total, t.offensive, t.defensive, t.misc, r.sensors, r.fireControl, r.armor,
                        r.size, r.maneuver, r.move1, r.move2, r.perks, r.flaws, r.weapons.size());
                std::vector<std::string> pk, fl;
                data_.perksAndFlaws(r, pk, fl);
                SDL_Log("   perks: %s | flaws: %s", join(pk, ',').c_str(), join(fl, ',').c_str());
            }
    if (getenv("HG2_LISTMSN")) {
        std::vector<std::string> ids = {"S2J"};
        for (size_t i = 0; i < ids.size() && i < 200; i++) {
            MsnInfo m;
            if (!data_.msn(ids[i], m)) continue;
            SDL_Log("MSN %s intro=%s outro=%s movie=%s next=%s alt=%s threat=%d..%d squad=%d", m.id.c_str(), m.introMovie.c_str(), m.outroMovie.c_str(), m.movie.c_str(), m.nextId.c_str(), m.altId.c_str(), m.threatMin, m.threatMax, m.squadMax);
            for (auto& n : {m.nextId, m.altId})
                if (!n.empty() && std::find(ids.begin(), ids.end(), upper(n.substr(0, n.find('.')))) == ids.end()) ids.push_back(upper(n.substr(0, n.find('.'))));
        }
        for (const char* p : {"H", "T"})
            for (int i = 1; i <= 10; i++) {
                MsnInfo m;
                char id[8];
                snprintf(id, sizeof id, "%s%02d", p, i);
                if (data_.msn(id, m)) SDL_Log("MSN %s intro=%s outro=%s", id, m.introMovie.c_str(), m.outroMovie.c_str());
            }
    }
    iaSquad_.assign(5, Slot{});
    iaEnemies_.assign(5, Slot{});
    const char* squadNames[] = {"", "Kage", "Pinter", "Temple", "Mailliaux"};
    for (int i = 0; i < 5; i++) {
        iaSquad_[size_t(i)].pilot = squadNames[i];
        iaSquad_[size_t(i)].used = i == 0;
        iaEnemies_[size_t(i)].pilot = "Enemy " + std::to_string(i + 1);
        iaEnemies_[size_t(i)].used = i < 2;
    }
    auto pick = [&](const char* n) {
        const Chassis* c = data_.findChassis(n);
        return c ? c->index : 0;
    };
    iaSquad_[0].chassis = pick("Jaguar");
    for (int i = 1; i < 5; i++) iaSquad_[size_t(i)].chassis = pick("Hunter");
    iaEnemies_[0].chassis = pick("Black Mamba"), iaEnemies_[1].chassis = pick("Iguana");
    for (int i = 2; i < 5; i++) iaEnemies_[size_t(i)].chassis = pick("Warrior");
    return true;
}

void Shell::go(Scr s) {
    threatDlg_.clear();
    if (movie_.m && !movie_.fullscreen) stopMovie();
    if (app_.audioOk) app_.audio.flushDialog();
    ui_.clearOverrides();
    popup_ = Popup{};
    scr_ = s;
    briefScroll_ = bioScroll_ = 0;
}

void Shell::background(const std::string& tga) { ui_.image(tga, 0, 0, 640, 480); }

void Shell::title(const std::string& t, float y) {
    TextStyle st;
    st.font = "s017016d.ttf", st.size = 10, st.just = "center", st.color = 0x4c8ef8ffu;
    ui_.textBox(t, {0, y, 640, 20}, st, st.color);
}

// ---------------------------------------------------------------- main loop
void Shell::run() {
    SDL_Window* win = app_.win;
    lastTick_ = SDL_GetTicksNS();
    // test automation: HG2_AUTOPLAY="frame:click:x,y;frame:key:name;frame:quit" (x, y in 640x480 units)
    std::vector<std::pair<int, std::string>> autoplay;
    if (const char* ap = getenv("HG2_AUTOPLAY"))
        for (auto& item : split(ap, ';')) {
            size_t c = item.find(':');
            if (c != std::string::npos) autoplay.push_back({atoi(item.c_str()), item.substr(c + 1)});
        }
    if (!autoplay.empty() && !getenv("HG2_MOVIES")) moviesEnabled_ = false;  // test runs skip the movies
    queueMovie("title.avi");
    int frameNo = 0;
    while (!quit_ && !app_.quit) {
        SDL_Event ev;
        float density = SDL_GetWindowPixelDensity(win);
        for (auto& [f, act] : autoplay) {
            if (f != frameNo) continue;
            int ww = 0, wh = 0;
            SDL_GetWindowSize(win, &ww, &wh);
            float sc = std::min(float(ww) / 640.f, float(wh) / 480.f), ox = (float(ww) - 640.f * sc) * 0.5f, oy = (float(wh) - 480.f * sc) * 0.5f;
            if (act.rfind("click:", 0) == 0) {
                float x = 0, y = 0;
                sscanf(act.c_str() + 6, "%f,%f", &x, &y);
                SDL_Event e{};
                e.type = SDL_EVENT_MOUSE_MOTION, e.motion.x = ox + x * sc, e.motion.y = oy + y * sc;
                SDL_PushEvent(&e);
                SDL_Event d{};
                d.type = SDL_EVENT_MOUSE_BUTTON_DOWN, d.button.button = SDL_BUTTON_LEFT;
                SDL_PushEvent(&d);
            } else if (act.rfind("up", 0) == 0) {
                SDL_Event u{};
                u.type = SDL_EVENT_MOUSE_BUTTON_UP, u.button.button = SDL_BUTTON_LEFT;
                SDL_PushEvent(&u);
            } else if (act.rfind("text:", 0) == 0) {
                ui_.textInput(act.substr(5));
            } else if (act == "enter") {
                ui_.key(SDLK_RETURN);
            } else if (act == "quit") {
                quit_ = true;
            } else if (act.rfind("shot:", 0) == 0) {  // X11 test runs: grab the screen with ImageMagick
#if !defined(_WIN32) && !defined(__APPLE__)
                std::string cmd = "import -window root " + act.substr(5);
                if (system(cmd.c_str()) != 0) SDL_Log("autoplay: screenshot failed");
#endif
            }
        }
        frameNo++;
        while (SDL_PollEvent(&ev)) {
            switch (ev.type) {
                case SDL_EVENT_QUIT: quit_ = true; break;
                case SDL_EVENT_MOUSE_MOTION: ui_.mouseMove(ev.motion.x * density, ev.motion.y * density); break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                    if (ev.button.button == SDL_BUTTON_LEFT) ui_.mouseDown();
                    break;
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    if (ev.button.button == SDL_BUTTON_LEFT) ui_.mouseUp();
                    break;
                case SDL_EVENT_MOUSE_WHEEL: ui_.wheel(ev.wheel.y); break;
                case SDL_EVENT_KEY_DOWN:
                    ui_.key(int(ev.key.key));
                    if (ev.key.key == SDLK_RETURN && (ev.key.mod & SDL_KMOD_ALT)) {
                        app_.settings.fullscreen = !app_.settings.fullscreen;
                        SDL_SetWindowFullscreen(win, app_.settings.fullscreen);
                    }
                    break;
                case SDL_EVENT_TEXT_INPUT: ui_.textInput(ev.text.text); break;
                default: break;
            }
        }
        if (quit_) break;
        if (app_.audioOk) {
            app_.audio.effectsVolume = app_.settings.shell;
            app_.audio.dialogVolume = app_.settings.dialog;
            app_.audio.update();
            if (!humOn_ && !(movie_.m && movie_.fullscreen) && movieQueue_.empty()) {  // the shell's background hum (hg2_asset_strings string6)
                app_.audio.startLoop("SHELHUM3.WAV", 0.35f);
                humOn_ = true;
            }
        }
        int w = 0, h = 0;
        SDL_GetWindowSizeInPixels(win, &w, &h);
        ui_.begin(w, h);
        frame();
        drawPopup();
        if (!(movie_.m && movie_.fullscreen)) ui_.cursor();
        ui_.end();
        FrameParams fp;
        fp.clearColor = Vec3(0, 0, 0);
        if (app_.r.beginFrame(fp)) app_.r.endFrame();
    }
    saveRoster();
    app_.settings.save(app_.prefDir + "settings.cfg");
}

bool Shell::screenshot(const std::string& screen, const std::string& png, float mx, float my) {
    auto parts = split(screen, ':');
    std::string s = parts.empty() ? "main" : parts[0];
    std::string arg = parts.size() > 1 ? parts[1] : "";
    if (s == "main") go(Scr::Main);
    else if (s == "single") go(Scr::Single);
    else if (s == "options") go(Scr::Options);
    else if (s == "sound") go(Scr::Sound);
    else if (s == "graphics") go(Scr::Graphics);
    else if (s == "controls") go(Scr::Controls);
    else if (s == "credits") go(Scr::Credits), creditScroll_ = 0;
    else if (s == "pilots") {
        if (roster_.empty()) roster_.push_back({"Grey Hawk", "Dark Cheetah"}), cur_ = 0;
        registering_ = arg == "new";
        go(Scr::Pilots);
    } else if (s == "load") {
        if (roster_.empty()) roster_.push_back({"Grey Hawk", "Dark Cheetah"});
        cur_ = 0;
        roster_[0].saves = {{"S2J", 'L', "2026-11-11 10:00", {}}, {"S2J", 'R', "2026-11-11 10:30", {}}, {"S1J", 'L', "2026-11-11 10:31", {}}};
        go(Scr::Load);
    } else if (s == "missions") {
        mode_ = arg == "historical" ? Mode::Historical : Mode::Training;
        missions_.clear();
        for (int i = 1; i <= 10; i++) {
            char id[8];
            snprintf(id, sizeof id, "%c%02d", mode_ == Mode::Training ? 'T' : 'H', i);
            MsnInfo m;
            if (data_.msn(id, m)) missions_.push_back(id);
        }
        go(Scr::Missions);
    } else if (s == "briefing" || s == "squad" || s == "gear" || s == "debrief") {
        mode_ = arg.empty() || arg[0] == 'S' ? Mode::Campaign : arg[0] == 'H' ? Mode::Historical : Mode::Training;
        if (mode_ == Mode::Campaign) {
            if (roster_.empty()) roster_.push_back({"Grey Hawk", "Dark Cheetah"});
            cur_ = 0;
        }
        openMission(arg.empty() ? "S01" : arg);
        if (s == "debrief") {
            result_ = MissionResult{};
            result_.outcome = parts.size() > 2 ? MissionResult::Outcome(atoi(parts[2].c_str())) : MissionResult::Success;
            result_.kills = 7, result_.shots = 120, result_.hits = 66, result_.time = 754;
            haveResult_ = true;
        }
        go(s == "briefing" ? Scr::Briefing : s == "squad" ? Scr::Squad : s == "gear" ? Scr::Gear : Scr::Debrief);
    } else if (s == "multi") {
        mode_ = Mode::Multi, go(Scr::Multi);
    } else if (s == "instant") {
        mode_ = Mode::Instant;
        go(Scr::Instant);
    }
    int w = app_.r.width(), h = app_.r.height();
    if (parts.size() > 2 && parts[2] == "play" && startMovie(msn_.movie, true, false))
        movie_.t0 -= Uint64(atof(parts.size() > 3 ? parts[3].c_str() : "5") * 1e9);  // briefing:<msn>:play:<seconds>
    else if (s == "movie" && startMovie(arg, false, true))
        movie_.t0 -= Uint64(atof(parts.size() > 2 ? parts[2].c_str() : "5") * 1e9);  // movie:<file>:<seconds>
    // two frames: the first creates textures and fonts, the second draws with hover state settled
    for (int i = 0; i < 2; i++) {
        ui_.begin(w, h);
        ui_.mouseMove(mx * ui_.scale() + (float(w) - 640.f * ui_.scale()) * 0.5f, my * ui_.scale() + (float(h) - 480.f * ui_.scale()) * 0.5f);
        frame();
        drawPopup();
        if (!(movie_.m && movie_.fullscreen)) ui_.cursor();
        ui_.end();
    }
    FrameParams fp;
    fp.clearColor = Vec3(0, 0, 0);
    if (!app_.r.beginFrame(fp)) return false;
    app_.r.endFrame();
    std::vector<uint8_t> px;
    int pw, ph;
    if (!app_.r.readback(px, pw, ph)) return false;
    return writePngFile(png, px, pw, ph);
}

void Shell::frame() {
    Uint64 now = SDL_GetTicksNS();
    float dt = std::min(0.1f, float(now - lastTick_) * 1e-9f);
    lastTick_ = now;
    creditT_ += dt;
    if (fullscreenMovie()) return;
    switch (scr_) {
        case Scr::Main: mainMenu(); break;
        case Scr::Single: singleMenu(); break;
        case Scr::Options: options(); break;
        case Scr::Sound: soundOptions(); break;
        case Scr::Graphics: graphicsOptions(); break;
        case Scr::Controls: controls(); break;
        case Scr::Credits: credits(); break;
        case Scr::Pilots: pilots(); break;
        case Scr::Load: loadGame(); break;
        case Scr::Missions: missionList(); break;
        case Scr::Briefing: briefing(); break;
        case Scr::Squad: squad(); break;
        case Scr::Gear: gear(); break;
        case Scr::Debrief: debrief(); break;
        case Scr::Instant: instantAction(); break;
        case Scr::Multi: multiplayer(); break;
    }
}

// ---------------------------------------------------------------- menus
void Shell::mainMenu() {
    background(kSplashArt);
    for (const char* w : {"splash_all_singles", "splash_multi_player", "splash_options", "splash_credit", "exit"}) ui_.window(w);
    if (ui_.clicked("splash_all_singles.btn")) go(Scr::Single);
    else if (ui_.clicked("splash_multi_player.enter")) mode_ = Mode::Multi, go(Scr::Multi);
    else if (ui_.clicked("splash_options.escape")) back_ = Scr::Main, go(Scr::Options);
    else if (ui_.clicked("splash_credit.btn")) creditScroll_ = 0, creditT_ = 0, go(Scr::Credits);
    else if (ui_.clicked("exit.exit")) quit_ = true;
}

void Shell::singleMenu() {
    background(kSplashArt);
    for (const char* w : {"splash_single_player", "splash_single_historical", "splash_training", "splash_instant_action", "splash_back"}) ui_.window(w);
    auto list = [&](Mode m, char prefix) {
        mode_ = m;
        missions_.clear();
        for (int i = 1; i <= 10; i++) {
            char id[8];
            snprintf(id, sizeof id, "%c%02d", prefix, i);
            MsnInfo info;
            if (data_.msn(id, info)) missions_.push_back(id);
        }
        listSel_ = 0, listScroll_ = 0;
        go(Scr::Missions);
    };
    if (ui_.clicked("splash_single_player.enter")) mode_ = Mode::Campaign, registering_ = roster_.empty(), go(Scr::Pilots);
    else if (ui_.clicked("splash_single_historical.enter")) list(Mode::Historical, 'H');
    else if (ui_.clicked("splash_training.btn")) list(Mode::Training, 'T');
    else if (ui_.clicked("splash_instant_action.enter")) mode_ = Mode::Instant, go(Scr::Instant);
    else if (ui_.clicked("splash_back.btn") || ui_.keyPressed(SDLK_ESCAPE)) go(Scr::Main);
}

void Shell::options() {
    ui_.window("escape");
    if (ui_.clicked("escape.bkgnd.splash") || ui_.keyPressed(SDLK_ESCAPE)) go(back_ == Scr::Options ? Scr::Main : back_ == Scr::Main ? Scr::Main : back_);
    else if (ui_.clicked("escape.bkgnd.graphics")) go(Scr::Graphics);
    else if (ui_.clicked("escape.bkgnd.sound")) go(Scr::Sound);
    else if (ui_.clicked("escape.bkgnd.control_config")) go(Scr::Controls);
    else if (ui_.clicked("escape.bkgnd.exit")) quit_ = true;
}

bool Shell::slider(const UiRect& r, float& v) {
    // HORIZSCROLLBAR art: track SSBR006X, arrows SSAR003/004, thumb SSBT002
    float before = v;
    ui_.image("SSBR006X.TGA", r.x + 12, r.y, r.w - 24, 12, 0xffffffffu, "tile");
    UiRect dec{r.x, r.y, 12, 12}, inc{r.x + r.w - 12, r.y, 12, 12}, track{r.x + 12, r.y - 4, r.w - 24, 20};
    ui_.image(dec.contains(ui_.mouseX(), ui_.mouseY()) ? "SSAR003M.TGA" : "SSAR003U.TGA", dec.x, dec.y);
    ui_.image(inc.contains(ui_.mouseX(), ui_.mouseY()) ? "SSAR004M.TGA" : "SSAR004U.TGA", inc.x, inc.y);
    std::string id = "#slider" + std::to_string(int(r.x)) + "," + std::to_string(int(r.y));
    if (ui_.mousePressed() && track.contains(ui_.mouseX(), ui_.mouseY())) dragging_ = id;
    if (!ui_.mouseHeld() && !ui_.mousePressed()) {
        if (dragging_ == id) ui_.playSound("scrollbar.wav");
        if (dragging_ == id) dragging_.clear();
    }
    if (dragging_ == id) v = std::clamp((ui_.mouseX() - track.x - 5.5f) / std::max(1.f, track.w - 11.f), 0.f, 1.f);
    if (ui_.mousePressed() && dec.contains(ui_.mouseX(), ui_.mouseY())) v = std::max(0.f, v - 0.05f), ui_.playSound("scrollbar.wav");
    if (ui_.mousePressed() && inc.contains(ui_.mouseX(), ui_.mouseY())) v = std::min(1.f, v + 0.05f), ui_.playSound("scrollbar.wav");
    ui_.image("SSBT002U.TGA", track.x + v * (track.w - 11.f), r.y);
    return v != before;
}

void Shell::soundOptions() {
    static Settings saved;
    static bool editing = false;
    if (!editing) saved = app_.settings, editing = true;
    for (const char* k : {"music", "dialog", "effects", "shell_volume"}) ui_.hidden[std::string("escape_sound_window.bkgnd.") + k + "_ranger"] = true;
    for (const char* k : {"shell_volume_1", "shell_volume_2", "shell_volume_3", "shell_volume_4"}) ui_.hidden[std::string("escape_sound_window.bkgnd.") + k] = true;
    struct Row { const char* key; float* v; };
    Row rows[] = {{"music", &app_.settings.music}, {"dialog", &app_.settings.dialog}, {"effects", &app_.settings.effects}, {"shell_volume", &app_.settings.shell}};
    for (auto& row : rows) ui_.text[std::string("escape_sound_window.bkgnd.") + row.key + "_value"] = std::to_string(int(std::lround(*row.v * 100)));
    ui_.window("escape_sound_window");
    for (auto& row : rows) {
        UiRect r = ui_.rectOf(std::string("escape_sound_window.bkgnd.") + row.key + "_ranger");
        if (r.w < 20) r.w = 200, r.x = 220;
        r.h = 12;
        if (slider(r, *row.v)) {
            app_.applyVolumes();
            if (std::string(row.key) == "shell_volume") ui_.shellVolume = app_.settings.shell;
        }
    }
    if (ui_.clicked("escape_sound_window.bkgnd.accept")) {
        editing = false;
        app_.settings.save(app_.prefDir + "settings.cfg");
        go(Scr::Options);
    } else if (ui_.clicked("escape_sound_window.bkgnd.cancel") || ui_.keyPressed(SDLK_ESCAPE)) {
        editing = false;
        app_.settings = saved;
        app_.applyVolumes();
        go(Scr::Options);
    }
}

void Shell::graphicsOptions() {
    static Settings saved;
    static bool editing = false;
    if (!editing) saved = app_.settings, editing = true;
    const std::string b = "escape_graphics_window.bkgnd.";
    // effects that exist in this port: particle density and the window mode
    for (const char* k : {"shadows_title", "shadows_off", "shadows_reduce", "shadows_full", "shadows_ranger", "detail_texture_title",
                          "detail_texture_off", "detail_texture_on", "detail_texture_ranger", "trilinear_filtering_title",
                          "trilinear_filtering_off", "trilinear_filtering_on", "trilinear_filtering_ranger", "dynamic_lighting_title",
                          "dynamic_lighting_off", "dynamic_lighting_on", "dynamic_lighting_ranger", "run_profiler_dropdown", "run_profiler_text",
                          "32_bit_color_warning", "particles_ranger", "screen_resolution_dropdown"})
        ui_.hidden[b + k] = true;
    ui_.text[b + "screen_resolution_text"] = "DISPLAY MODE";
    ui_.window("escape_graphics_window");
    UiRect pr = ui_.rectOf(b + "particles_ranger");
    if (pr.w < 20) pr = {120, 130, 200, 12};
    pr.h = 12;
    float v = float(app_.settings.particles) / 2.f;
    if (slider(pr, v)) app_.settings.particles = int(std::lround(v * 2.f));
    v = float(app_.settings.particles) / 2.f;
    UiRect dr = ui_.rectOf(b + "screen_resolution_dropdown");
    if (dr.w < 20) dr = {240, 380, 160, 20};
    dr.h = 20;
    std::vector<std::string> modes = {"WINDOW", "FULL SCREEN"};
    TextStyle st;
    st.font = "s017016d.ttf", st.size = 8, st.just = "center", st.vcenter = true;
    ui_.image("SBTN026U.TGA", dr.x, dr.y, dr.w, dr.h, 0xffffffffu, "stretch");
    ui_.textBox(modes[app_.settings.fullscreen ? 1 : 0], dr, st, 0x8cc8ffffu);
    if (ui_.mouseReleased() && dr.contains(ui_.mouseX(), ui_.mouseY())) {
        app_.settings.fullscreen = !app_.settings.fullscreen;
        ui_.playSound("general.wav");
    }
    // the port's own options (right column): anti-aliasing, the movie filter, the movies' 4:3 screen
    {
        TextStyle hs;
        hs.font = "s017016d.ttf", hs.size = 8, hs.just = "center", hs.color = 0x4c8ef8ffu;
        auto option = [&](float y, const char* title, const std::string& value, auto onClick) {
            UiRect tr{400, y, 170, 14}, br{400, y + 16, 170, 20};
            ui_.textBox(title, tr, hs, hs.color);
            ui_.image("SBTN026U.TGA", br.x, br.y, br.w, br.h, 0xffffffffu, "stretch");
            ui_.textBox(value, br, st, 0x8cc8ffffu);
            if (ui_.mouseReleased() && br.contains(ui_.mouseX(), ui_.mouseY())) onClick(), ui_.playSound("general.wav");
        };
        int aa = app_.settings.msaa;
        option(120, "ANTI-ALIASING", aa <= 1 ? "OFF" : std::to_string(aa) + "X MSAA",
               [&] { app_.settings.msaa = aa <= 1 ? 2 : aa >= 8 ? 1 : aa * 2; });
        static const char* filters[] = {"OFF", "CRT", "INTERLACED"};
        option(170, "MOVIE FILTER", filters[std::clamp(app_.settings.movieFilter, 0, 2)],
               [&] { app_.settings.movieFilter = (app_.settings.movieFilter + 1) % 3; });
        option(220, "MOVIE SCREEN", app_.settings.movie43 ? "4:3 (ORIGINAL)" : "FILL WINDOW",
               [&] { app_.settings.movie43 = !app_.settings.movie43; });
    }
    if (ui_.clicked(b + "accept")) {
        editing = false;
        app_.r.setSamples(app_.settings.msaa);
        SDL_SetWindowFullscreen(app_.win, app_.settings.fullscreen);
        app_.settings.save(app_.prefDir + "settings.cfg");
        go(Scr::Options);
    } else if (ui_.clicked(b + "cancel") || ui_.keyPressed(SDLK_ESCAPE)) {
        editing = false;
        app_.settings = saved;
        go(Scr::Options);
    }
}

void Shell::controls() {
    ui_.image("Sbakescp.tga", 0, 0, 640, 480);
    title("CONTROLS", 52);
    static const char* rows[][2] = {
        {"Walk forward / back", "W / S  (keypad 8 / 2)"}, {"Turn", "A / D  (keypad 4 / 6)"}, {"Sidestep", "Q / E  (keypad 7 / 9)"},
        {"Run", "Left Shift"}, {"Aim / torso twist", "Mouse"}, {"Fire", "Left button / Left Ctrl"},
        {"Select weapon", "1 - 8, Tab: next"}, {"Crouch / prone", "Z  (keypad /)"}, {"Stand up", "X  (keypad *)"},
        {"Jump", "Space  (keypad Enter)"}, {"Skates (SMS)", "R  (keypad 0 / Ins)"}, {"Cockpit / chase view", "C"},
        {"Camera orbit / tilt", "Arrow keys"}, {"Chase camera zoom", "Wheel, Home / End"}, {"Next / previous nav point", "N / B"},
        {"Skip cut scene", "Space"}, {"Music on / off", "M"}, {"Pause menu", "Esc"},
    };
    TextStyle a, k;
    a.size = k.size = 8, a.color = k.color = 0x4c8ef8ffu;
    k.color = 0x8cc8ffffu;
    float y = 84;
    for (auto& r : rows) {
        ui_.textBox(r[0], {110, y, 220, 16}, a, a.color);
        ui_.textBox(r[1], {330, y, 220, 16}, k, k.color);
        y += 16;
    }
    // mouse settings
    TextStyle h;
    h.font = "s017016d.ttf", h.size = 9, h.color = 0x4c8ef8ffu;
    ui_.textBox("MOUSE SPEED", {110, y + 10, 200, 16}, h, h.color);
    float v = (app_.settings.mouseSpeed - 0.25f) / 2.75f;
    if (slider({330, y + 12, 180, 12}, v)) app_.settings.mouseSpeed = 0.25f + v * 2.75f;
    ui_.textBox(app_.settings.invertMouse ? "INVERT MOUSE Y: ON" : "INVERT MOUSE Y: OFF", {110, y + 32, 300, 16}, h, h.color);
    UiRect inv{110, y + 32, 200, 16};
    if (ui_.mouseReleased() && inv.contains(ui_.mouseX(), ui_.mouseY())) app_.settings.invertMouse = !app_.settings.invertMouse, ui_.playSound("general.wav");
    ui_.textBox(app_.settings.externalView ? "START IN CHASE VIEW: ON" : "START IN CHASE VIEW: OFF", {110, y + 50, 300, 16}, h, h.color);
    UiRect ext{110, y + 50, 220, 16};
    if (ui_.mouseReleased() && ext.contains(ui_.mouseX(), ui_.mouseY())) app_.settings.externalView = !app_.settings.externalView, ui_.playSound("general.wav");
    ui_.window("low_accept_sound");
    ui_.text["low_accept_sound.btn"] = "ACCEPT";
    if (ui_.clicked("low_accept_sound.btn") || ui_.keyPressed(SDLK_ESCAPE)) {
        app_.settings.save(app_.prefDir + "settings.cfg");
        go(Scr::Options);
    }
}

void Shell::credits() {
    background(kSplashArt);
    // CREDIT.TXT: pages split by <E>; <t:font> <p:size> <c:r,g,b> ... </t> </p> </c> style the lines
    static std::vector<std::string> pages;
    if (pages.empty()) {
        std::string t = data_.text("CREDIT.TXT");
        size_t a = 0;
        while (a < t.size()) {
            size_t e = t.find("<E>", a);
            if (e == std::string::npos) e = t.size();
            pages.push_back(t.substr(a, e - a));
            a = e + 3;
        }
    }
    const float pageTime = 6.5f;
    int page = int(creditT_ / pageTime);
    if (ui_.mouseReleased() && !ui_.rectOf("credit_back").contains(ui_.mouseX(), ui_.mouseY())) creditT_ = float(page + 1) * pageTime, page++;
    if (pages.empty() || page >= int(pages.size())) creditT_ = 0, page = 0;
    float local = creditT_ - float(page) * pageTime;
    float alpha = std::min({1.f, local / 0.6f, (pageTime - local) / 0.6f});
    if (!pages.empty()) {
        TextStyle def;
        def.font = "Eum_____.ttf", def.size = 10, def.color = 0x4c8ef8ffu;
        TextStyle cur = def;
        float y = 40;
        std::string line;
        std::string body = pages[size_t(page)];
        size_t i = 0;
        auto flush = [&]() {
            std::string l = line;
            while (!l.empty() && (l.back() == '\r' || l.back() == ' ')) l.pop_back();
            uint32_t c = (cur.color & 0xffffff00u) | uint32_t(std::clamp(alpha, 0.f, 1.f) * 255.f);
            if (!l.empty()) ui_.textBox(l, {40, y, 380, 30}, cur, c);
            y += ui_.lineHeight(cur);
            line.clear();
        };
        TextStyle lineStyle = cur;
        bool lineStarted = false;
        while (i < body.size() && y < 440) {
            char ch = body[i];
            if (ch == '<') {
                size_t e = body.find('>', i);
                if (e == std::string::npos) break;
                std::string tag = body.substr(i + 1, e - i - 1);
                i = e + 1;
                if (tag.size() > 2 && tag[0] == 't' && tag[1] == ':') cur.font = tag.substr(2);
                else if (tag.size() > 2 && tag[0] == 'p' && tag[1] == ':') cur.size = atoi(tag.c_str() + 2);
                else if (tag.size() > 2 && tag[0] == 'c' && tag[1] == ':') {
                    int r = 0, g = 0, b = 0;
                    sscanf(tag.c_str() + 2, "%d,%d,%d", &r, &g, &b);
                    cur.color = uint32_t(r) << 24 | uint32_t(g) << 16 | uint32_t(b) << 8 | 0xffu;
                } else if (tag == "/t") cur.font = def.font;
                else if (tag == "/p") cur.size = def.size;
                else if (tag == "/c") cur.color = def.color;
                if (!lineStarted) lineStyle = cur;
                continue;
            }
            if (ch == '\n') {
                TextStyle after = cur;
                cur = lineStyle;
                flush();
                cur = after, lineStyle = cur, lineStarted = false;
                i++;
                continue;
            }
            if (!lineStarted) lineStyle = cur, lineStarted = true;
            line += ch;
            i++;
        }
        if (!line.empty()) cur = lineStyle, flush();
    }
    ui_.window("credit_back");
    if (ui_.clicked("credit_back.btn") || ui_.keyPressed(SDLK_ESCAPE)) go(Scr::Main);
}

// ---------------------------------------------------------------- popup lists
int Shell::dropdown(const std::string& path, const std::vector<std::string>& items, int sel, const std::string& id) {
    if (popup_.picked >= 0 && popup_.id == id) {
        int p = popup_.picked;
        popup_ = Popup{};
        return p;
    }
    if (ui_.clicked(path) && !items.empty()) {
        popup_ = Popup{};
        popup_.id = id, popup_.anchor = ui_.rectOf(path), popup_.items = items, popup_.sel = sel, popup_.open = true;
        popup_.scroll = std::max(0, sel - 4);
    }
    return -1;
}

void Shell::drawPopup() {
    if (!popup_.open) return;
    const float ih = 15;
    int visible = std::min<int>(int(popup_.items.size()), 12);
    float w = std::max(popup_.anchor.w, 150.f), h = float(visible) * ih + 6;
    float x = popup_.anchor.x, y = popup_.anchor.y + popup_.anchor.h;
    if (y + h > 470) y = std::max(10.f, popup_.anchor.y - h);
    if (x + w > 630) x = 630 - w;
    ui_.image("Sfra013x.TGA", x, y, w, h, 0xffffffffu, "stretch");
    ui_.rect(x, y, w, h, 0x00081890u);
    ui_.frame(x, y, w, h, 0x3a6eb0ffu);
    TextStyle st;
    st.size = 7, st.font = "eub_____.ttf", st.insetX = 6, st.vcenter = true;
    UiRect area{x, y + 3, w, h - 6};
    int maxScroll = std::max(0, int(popup_.items.size()) - visible);
    if (area.contains(ui_.mouseX(), ui_.mouseY()) && ui_.wheelDelta() != 0) popup_.scroll -= ui_.wheelDelta() > 0 ? 1 : -1;
    popup_.scroll = std::clamp(popup_.scroll, 0, maxScroll);
    for (int i = 0; i < visible; i++) {
        int idx = popup_.scroll + i;
        UiRect r{x + 2, y + 3 + float(i) * ih, w - (maxScroll ? 16.f : 4.f), ih};
        bool hov = r.contains(ui_.mouseX(), ui_.mouseY());
        if (hov || idx == popup_.sel) ui_.rect(r.x, r.y, r.w, r.h, hov ? 0x2a64c0b0u : 0x1a3c7890u);
        ui_.textBox(popup_.items[size_t(idx)], r, st, hov ? 0xffffffffu : 0x8cc8ffffu);
        if (hov && ui_.mouseReleased()) {
            popup_.picked = idx, popup_.open = false;
            ui_.playSound("general.wav");
            return;
        }
    }
    if (maxScroll > 0) {
        float t = float(popup_.scroll) / float(maxScroll);
        ui_.rect(x + w - 12, y + 3, 10, h - 6, 0x0a1a3080u);
        ui_.rect(x + w - 12, y + 3 + t * (h - 20), 10, 14, 0x3a6eb0ffu);
    }
    if ((ui_.mouseReleased() && !area.contains(ui_.mouseX(), ui_.mouseY()) && !popup_.anchor.contains(ui_.mouseX(), ui_.mouseY())) ||
        ui_.keyPressed(SDLK_ESCAPE))
        popup_.open = false, popup_.id.clear();
}

// ---------------------------------------------------------------- campaign: pilots
void Shell::pilots() {
    background(kCharArt);
    std::vector<std::string> squadChassis;
    std::vector<int> squadIdx;
    for (auto& c : data_.chassis)
        if (c.squad) squadChassis.push_back(c.name), squadIdx.push_back(c.index);
    if (squadChassis.empty())
        for (auto& c : data_.chassis) squadChassis.push_back(c.name), squadIdx.push_back(c.index);
    if (!registering_) ui_.window("character_listbox_title");
    if (registering_) {
        newChassis_ = std::clamp(newChassis_, 0, int(squadIdx.size()) - 1);
        const Chassis& ch = data_.chassis[size_t(squadIdx[size_t(newChassis_)])];
        ui_.hidden["character_register.name_input"] = false;
        ui_.window("character_register");
        UiRect nr = ui_.rectOf("character_register.name_input");
        TextStyle st = ui_.style(ui_.ldl().find("character_register.name_input"));
        bool enter = ui_.textField(newName_, nr, st, 20, true);
        ui_.text["character_initchassis_btn.btn"] = ch.name;
        ui_.window("character_initchassis_btn");
        int pick = dropdown("character_initchassis_btn.btn", squadChassis, newChassis_, "regchassis");
        if (pick >= 0) newChassis_ = pick, notesScroll_ = 0;
        ui_.hidden["character_initchassis_notes.tb"] = false;
        UiRect notes = ui_.rectOf("character_initchassis_notes");
        ui_.image("SDBX016X.TGA", notes.x, notes.y);
        TextStyle bt = ui_.style(ui_.ldl().find("block_text"));
        bt.size = 7, bt.wrap = true, bt.insetX = 6, bt.insetY = 4;
        ui_.scrollText(ch.description, {notes.x, notes.y, 174, 150}, bt, notesScroll_);
        ui_.image(ch.picture, 105, 110, 188, 220);
        ui_.window("back_character_scrn");
        ui_.window("low_register_pilot");
        bool nameOk = !newName_.empty() && std::none_of(roster_.begin(), roster_.end(), [&](const CampaignPilot& p) { return lower(p.name) == lower(newName_); });
        ui_.disabled["low_register_pilot.btn"] = !nameOk;
        if ((ui_.clicked("low_register_pilot.btn") || enter) && nameOk) {
            CampaignPilot p;
            p.name = newName_, p.chassis = ch.name;
            roster_.push_back(p);
            cur_ = int(roster_.size()) - 1;
            newName_.clear();
            registering_ = false;
            saveRoster();
            SDL_StopTextInput(app_.win);
        } else if (ui_.clicked("back_character_scrn.back") || ui_.keyPressed(SDLK_ESCAPE)) {
            registering_ = false;
            SDL_StopTextInput(app_.win);
            if (roster_.empty()) go(Scr::Single);
        }
        if (registering_ && app_.win && !SDL_TextInputActive(app_.win)) SDL_StartTextInput(app_.win);
        return;
    }
    // pilot list
    std::vector<std::string> names;
    for (auto& p : roster_) names.push_back(p.name);
    if (cur_ >= int(roster_.size())) cur_ = int(roster_.size()) - 1;
    if (cur_ < 0 && !roster_.empty()) cur_ = 0;
    ui_.window("character_listbox");
    int c = ui_.listbox("character_listbox.lb_name", names, cur_, pilotScroll_, "character_listbox_text");
    if (c >= 0) cur_ = c;
    CampaignPilot* p = pilot();
    if (p) {
        const Chassis* ch = data_.findChassis(p->chassis);
        if (ch) ui_.image(ch->picture, 105, 110, 188, 220);
        // record: difficulty, sorties, kills, accuracy, times killed
        const std::string b = "character_stats.bg.";
        char buf[32];
        snprintf(buf, sizeof buf, "%d%%", int(std::lround(p->difficulty * 100)));
        ui_.text[b + "diff_val"] = buf;
        ui_.text[b + "kills_val"] = std::to_string(p->kills);
        ui_.text[b + "sorties_val"] = std::to_string(p->sorties);
        ui_.text[b + "killed_val"] = std::to_string(p->deaths);
        snprintf(buf, sizeof buf, "%d%%", p->shots ? int(100.f * float(p->hits) / float(p->shots)) : 0);
        ui_.text[b + "acc_val"] = buf;
    }
    bool started = p && (!p->saves.empty() || p->mission != kCampaignStart);
    for (const char* w : {"back_character_scrn", "low_new_pilot", "low_delete_pilot", "low_load_game"}) ui_.window(w);
    ui_.disabled["low_delete_pilot.btn"] = !p;
    ui_.disabled["low_load_game.btn"] = !p || p->saves.empty();
    if (started) ui_.window("low_continue");
    else ui_.window("low_new_game"), ui_.disabled["low_new_game.btn"] = !p;
    if (confirmDelete_ && p) {
        ui_.text["dlg_delete_pilot.pane.msg"] = "Delete pilot " + p->name + "?";
        ui_.window("dlg_delete_pilot");
        if (ui_.clicked("dlg_delete_pilot.pane.yes")) {
            roster_.erase(roster_.begin() + cur_);
            cur_ = std::min(cur_, int(roster_.size()) - 1);
            confirmDelete_ = false;
            saveRoster();
        } else if (ui_.clicked("dlg_delete_pilot.pane.no") || ui_.keyPressed(SDLK_ESCAPE)) {
            confirmDelete_ = false;
        }
        return;
    }
    if (ui_.clicked("back_character_scrn.back") || ui_.keyPressed(SDLK_ESCAPE)) go(Scr::Single);
    else if (ui_.clicked("low_new_pilot.btn")) registering_ = true, newName_.clear();
    else if (ui_.clicked("low_delete_pilot.btn") && p) confirmDelete_ = true;
    else if (ui_.clicked("low_load_game.btn") && p) loadSel_ = int(p->saves.size()) - 1, loadScroll_ = std::max(0, loadSel_ - 10), go(Scr::Load);
    else if ((ui_.clicked("low_new_game.btn") || ui_.clicked("low_continue.btn")) && p) {
        bool fresh = !started || ui_.clicked("low_new_game.btn");
        if (!started) p->mission = kCampaignStart;
        if (openMission(p->mission)) {
            go(Scr::Briefing);
            if (fresh) queueMovie(msn_.introMovie);
        }
    }
}

void Shell::loadGame() {
    background(kCharArt);
    CampaignPilot* p = pilot();
    if (!p) {
        go(Scr::Pilots);
        return;
    }
    std::vector<std::string> items;
    for (auto& s : p->saves) {
        MsnInfo m;
        std::string t = data_.msn(s.mission, m) ? m.title : s.mission;
        items.push_back(std::string(s.kind == 'L' ? "Launch: " : "Return: ") + t);
    }
    ui_.text["missions_past.lb.header.switch0"] = "SAVED GAMES";
    ui_.window("missions_past");
    int c = ui_.listbox("missions_past.lb", items, loadSel_, loadScroll_);
    if (c >= 0) loadSel_ = c;
    loadSel_ = std::clamp(loadSel_, 0, int(p->saves.size()) - 1);
    if (!p->saves.empty()) {
        const Save& s = p->saves[size_t(loadSel_)];
        std::string id = s.mission;
        // stills: SSAVESnn (launch) / SSAVRSnn (return)
        std::string num = id.size() >= 3 ? id.substr(1) : id;
        std::string img = std::string(s.kind == 'L' ? "SSAVES" : "SSAVRS") + num + ".TGA";
        int iw, ih;
        if (!ui_.imageSize(img, iw, ih)) img = "SSAVES" + num + ".TGA";
        ui_.image("SFRA016X.TGA", 262, 58);
        ui_.image(img, 262, 58, 300, 130);
        std::string txt = trimNl(data_.text(id + (s.kind == 'L' ? "LANCH.TXT" : "RETRN.TXT")));
        TextStyle st;
        st.size = 8, st.wrap = true, st.insetX = 10, st.insetY = 8;
        ui_.image("SDBX001X.TGA", 262, 210);
        ui_.textBox(txt + "\n\nSaved " + s.date, {262, 210, 300, 179}, st, 0x4c8ef8ffu);
    }
    ui_.window("back");
    ui_.window("low_dbrf_continue");
    ui_.text["low_dbrf_continue.btn"] = "LOAD GAME";
    ui_.disabled["low_dbrf_continue.btn"] = p->saves.empty();
    if (ui_.clicked("back.back") || ui_.keyPressed(SDLK_ESCAPE)) go(Scr::Pilots);
    else if (ui_.clicked("low_dbrf_continue.btn") && !p->saves.empty()) {
        const Save& s = p->saves[size_t(loadSel_)];
        p->lost = s.lost;
        MsnInfo m;
        // a return save resumes at the next mission's briefing
        std::string id = s.mission;
        if (s.kind == 'R' && data_.msn(id, m) && !m.nextId.empty() && lower(m.nextId) != "end") id = m.nextId;
        p->mission = id;
        if (openMission(id)) go(Scr::Briefing);
    }
}

// ---------------------------------------------------------------- mission lists (historical / training)
std::string Shell::missionName(const MsnInfo& m) const {
    if (!m.title.empty() && m.title != "TEMP TITLE") return m.title;
    // historical missions carry a placeholder title: use the briefing's location line
    std::string b = m.briefing;
    size_t p = b.find("Location");
    if (p != std::string::npos) {
        size_t c = b.find(':', p), e = b.find_first_of("\n,.", c);
        if (c != std::string::npos) {
            std::string loc = b.substr(c + 1, e == std::string::npos ? std::string::npos : e - c - 1);
            while (!loc.empty() && loc[0] == ' ') loc.erase(0, 1);
            if (!loc.empty()) return loc;
        }
    }
    return m.id;
}

void Shell::missionList() {
    background(kTrideoArt);
    std::vector<std::string> items;
    std::vector<MsnInfo> infos;
    for (auto& id : missions_) {
        MsnInfo m;
        data_.msn(id, m);
        infos.push_back(m);
        items.push_back(std::to_string(items.size() + 1) + ". " + missionName(m));
    }
    bool training = mode_ == Mode::Training;
    std::string lb = training ? "missions_historical" : "missions_historical";
    ui_.text[lb + ".lb.header.switch0"] = training ? "TRAINING" : "HISTORICAL";
    ui_.window(lb);
    int c = ui_.listbox(lb + ".lb", items, listSel_, listScroll_);
    if (c >= 0 && c != listSel_) listSel_ = c, briefScroll_ = 0;
    listSel_ = std::clamp(listSel_, 0, std::max(0, int(items.size()) - 1));
    if (!infos.empty()) {
        const MsnInfo& m = infos[size_t(listSel_)];
        ui_.text["ln_brf_video_title.static"] = missionName(m);
        ui_.window("ln_brf_video_title");
        ui_.hidden["ln_brf_video.display"] = true;
        ui_.image("SFRA016X.TGA", 262, 58);
        ui_.image(m.image, 262, 58, 300, 130);
        ui_.window("ln_brf_summary_title");
        UiRect r = ui_.rectOf("ln_brf_summary");
        ui_.image("SDBX001X.TGA", r.x, r.y);
        TextStyle st;
        st.size = 7, st.wrap = true, st.insetX = 10, st.insetY = 7;
        ui_.scrollText(m.briefing, {r.x, r.y, 300, r.h}, st, briefScroll_);
    }
    ui_.window("back");
    std::string go2 = training ? "low_launch_training" : "low_hist_game";
    ui_.window(go2);
    if (ui_.clicked("back.back") || ui_.keyPressed(SDLK_ESCAPE)) go(Scr::Single);
    else if (ui_.clicked(go2 + ".btn") && !missions_.empty()) {
        if (openMission(missions_[size_t(listSel_)])) {
            if (training && msn_.squadMax <= 1 && msn_.pilots.empty()) launch();
            else go(Scr::Briefing);
        }
    }
}

// ---------------------------------------------------------------- mission preparation
int Shell::defaultVariant(int chassis) const {
    if (chassis < 0 || size_t(chassis) >= data_.chassis.size()) return 0;
    const auto& v = data_.chassis[size_t(chassis)].variants;
    for (size_t i = 0; i < v.size(); i++)
        if (!v[i].hidden) return int(i);
    return 0;
}

int Shell::missionGear(const std::string& id) const {
    Bytes b;
    Mission m;
    if (!app_.fs.read("mission/" + id + ".ANN", b) || !loadMission(b, m)) return -1;
    for (auto& o : m.objects) {
        if (!o.userHeader || o.defIndex < 0 || size_t(o.defIndex) >= m.defs.size()) continue;
        std::string model = lower(m.defs[size_t(o.defIndex)].firstModel);
        for (int i = 0; i < int(data_.chassis.size()); i++) {
            Bytes g;
            if (!masterGear(app_.fs, i, g)) break;
            GearDef d;
            if (parseGearDef(g.data(), g.size(), d) && !d.geometry.empty() && lower(d.geometry[0]) == model) return i;
        }
    }
    return -1;
}

bool Shell::openMission(const std::string& id) {
    MsnInfo m;
    if (!data_.msn(id, m)) {
        SDL_Log("shell: briefing %s.MSN not found", id.c_str());
        return false;
    }
    msn_ = m;
    user_ = Slot{};
    CampaignPilot* p = pilot();
    if (mode_ == Mode::Campaign && p) {
        user_.pilot = p->name;
        const Chassis* c = data_.findChassis(p->chassis);
        user_.chassis = c ? c->index : -1;
        user_.variant = defaultVariant(user_.chassis);
        if (c)
            for (size_t i = 0; i < c->variants.size(); i++)
                if (c->variants[i].name == p->variant && !c->variants[i].hidden) user_.variant = int(i);
        applyRefit(user_, p->refit);
        applyRestat(user_, p->restat);
    } else {
        user_.pilot = "Commander";
        user_.chassis = missionGear(id);
        user_.variant = defaultVariant(user_.chassis);
    }
    if (!m.forcedChassis.empty()) {  // training missions put the user in a set gear
        if (const Chassis* c = data_.findChassis(m.forcedChassis)) {
            user_.chassis = c->index;
            for (size_t i = 0; i < c->variants.size(); i++)
                if (lower(c->variants[i].name) == lower(m.forcedVariant)) user_.variant = int(i);
        }
    }
    defaultSquad();
    return true;
}

void Shell::defaultSquad() {
    squad_.clear();
    auto add = [&](const std::string& name) {
        const Pilot* pp = data_.findPilot(name);
        Slot s;
        s.pilot = name;
        if (pp) {
            if (const Chassis* c = data_.findChassis(pp->chassis)) {
                s.chassis = c->index;
                s.variant = defaultVariant(c->index);
                for (size_t i = 0; i < c->variants.size(); i++)
                    if (lower(c->variants[i].name) == lower(pp->variant)) s.variant = int(i);
            }
        }
        if (s.chassis < 0) s.chassis = user_.chassis >= 0 ? user_.chassis : 0, s.variant = defaultVariant(s.chassis);
        squad_.push_back(s);
    };
    int room = std::max(0, msn_.squadMax - 1);
    CampaignPilot* p = pilot();
    auto lost = [&](const std::string& n) {
        return p && mode_ == Mode::Campaign && std::find(p->lost.begin(), p->lost.end(), n) != p->lost.end();
    };
    // required pilots first, then the last squad picked, then whoever is available
    for (size_t i = 0; i < msn_.pilots.size(); i++)
        if (i < msn_.pilotRequired.size() && msn_.pilotRequired[i] && !lost(msn_.pilots[i]) && int(squad_.size()) < room) add(msn_.pilots[i]);
    auto have = [&](const std::string& n) {
        return std::any_of(squad_.begin(), squad_.end(), [&](const Slot& s) { return s.pilot == n; });
    };
    if (p && mode_ == Mode::Campaign)
        for (auto& n : p->squad)
            if (int(squad_.size()) < room && !have(n) && !lost(n) && std::find(msn_.pilots.begin(), msn_.pilots.end(), n) != msn_.pilots.end()) add(n);
    std::vector<std::string> pool = msn_.pilots;
    if (msn_.fixedSquad && mode_ == Mode::Historical)
        for (auto& pp : data_.pilots) pool.push_back(pp.name);
    for (auto& n : pool)
        if (int(squad_.size()) < room && !have(n) && !lost(n)) add(n);
}

std::string Shell::slotChassisName(const Slot& s) const {
    if (s.chassis < 0 || size_t(s.chassis) >= data_.chassis.size()) return "Mission Gear";
    return data_.chassis[size_t(s.chassis)].name;
}

std::string Shell::slotVariantName(const Slot& s) const {
    if (s.chassis < 0 || size_t(s.chassis) >= data_.chassis.size()) return "Standard";
    const auto& v = data_.chassis[size_t(s.chassis)].variants;
    return s.variant >= 0 && size_t(s.variant) < v.size() ? v[size_t(s.variant)].name : "Standard";
}

bool Shell::slotRecord(const Slot& s, VarRecord& r) const {
    if (s.chassis < 0 || size_t(s.chassis) >= data_.chassis.size()) return false;
    const auto& v = data_.chassis[size_t(s.chassis)].variants;
    if (s.variant < 0 || size_t(s.variant) >= v.size()) return false;
    if (!data_.variantRecord(v[size_t(s.variant)].file, r)) return false;
    if (s.restat) {
        r.armor = s.rec.armor, r.sensors = s.rec.sensors, r.fireControl = s.rec.fireControl, r.maneuver = s.rec.maneuver;
        r.perks = s.rec.perks, r.flaws = s.rec.flaws;
    }
    if (s.refit) r = data_.withWeapons(r, s.weapons);
    return true;
}

GearChoice Shell::gearChoice(const Slot& s) const {
    GearChoice g;
    g.chassis = s.chassis;
    g.weapons = loadoutFor(s);
    VarRecord r;
    if (slotRecord(s, r)) {
        g.armor = r.armor, g.fireControl = r.fireControl;
        g.hasSystems = true, g.sensors = r.sensors, g.maneuver = r.maneuver, g.perks = r.perks, g.flaws = r.flaws;
    }
    return g;
}

bool Shell::slotThreat(const Slot& s, Threat& out) const {
    VarRecord r;
    if (!slotRecord(s, r)) return false;
    out = data_.threat(r);
    return true;
}

std::string Shell::restatString(const Slot& s) const {
    if (!s.restat) return {};
    char b[96];
    snprintf(b, sizeof b, "%d,%d,%d,%d,%08x,%08x", s.rec.armor, s.rec.sensors, s.rec.fireControl, s.rec.maneuver, s.rec.perks, s.rec.flaws);
    return b;
}

void Shell::applyRestat(Slot& s, const std::string& r) const {
    s.restat = false;
    unsigned p = 0, f = 0;
    if (!r.empty() && sscanf(r.c_str(), "%d,%d,%d,%d,%x,%x", &s.rec.armor, &s.rec.sensors, &s.rec.fireControl, &s.rec.maneuver, &p, &f) == 6)
        s.restat = true, s.rec.perks = p, s.rec.flaws = f;
}

std::vector<int> Shell::weaponsForHardpoint(int slot) {
    if (hardpointWeapons_.empty()) {
        std::map<int, std::set<int>> m;
        for (auto& ch : data_.chassis)
            for (auto& v : ch.variants) {
                std::vector<VarWeapon> l;
                if (!data_.variant(v.file, l)) continue;
                for (auto& w : l)
                    if (int idx = data_.weaponIndex(w.wpn); idx >= 0) m[w.slot].insert(idx);
            }
        for (auto& [k, s] : m) hardpointWeapons_[k].assign(s.begin(), s.end());
        hardpointWeapons_[-1];  // built
    }
    auto it = hardpointWeapons_.find(slot);
    return it == hardpointWeapons_.end() ? std::vector<int>{} : it->second;
}

std::string Shell::refitString(const Slot& s) const {
    if (!s.refit) return {};
    std::string r;
    for (auto& [slot, w] : s.weapons)
        if (w >= 0 && size_t(w) < data_.weaponFiles.size()) r += (r.empty() ? "" : ",") + std::to_string(slot) + ":" + data_.weaponFiles[size_t(w)];
    return r.empty() ? "-" : r;
}

void Shell::applyRefit(Slot& s, const std::string& r) const {
    s.refit = false, s.weapons.clear();
    if (r.empty()) return;
    s.refit = true;
    for (auto& e : split(r, ',')) {
        size_t c = e.find(':');
        if (c == std::string::npos) continue;
        int w = data_.weaponIndex(e.substr(c + 1));
        if (w >= 0) s.weapons.push_back({atoi(e.c_str()), w});
    }
}

std::vector<std::pair<int, int>> Shell::loadoutFor(const Slot& s) const {
    if (s.refit) return s.weapons;
    std::vector<std::pair<int, int>> out;
    if (s.chassis < 0 || size_t(s.chassis) >= data_.chassis.size()) return out;
    const auto& v = data_.chassis[size_t(s.chassis)].variants;
    if (s.variant < 0 || size_t(s.variant) >= v.size()) return out;
    std::vector<VarWeapon> l;
    if (!data_.variant(v[size_t(s.variant)].file, l)) return out;
    for (auto& w : l) {
        int idx = data_.weaponIndex(w.wpn);
        if (idx >= 0) out.push_back({w.slot, idx});
    }
    return out;
}

void Shell::playBriefingAudio() {
    if (msn_.movie.empty()) return;
    if (startMovie(msn_.movie, true, false)) return;
    // no decodable video: the narration alone
    if (!app_.audioOk) return;
    std::string p = findMovie(msn_.movie, true);
    Bytes b;
    SDL_AudioSpec spec;
    std::vector<uint8_t> pcm;
    if (!p.empty() && readFile(p, b) && aviAudio(b, spec, pcm)) app_.audio.playPcm(spec, pcm.data(), pcm.size(), 1.f);
    else SDL_Log("shell: no soundtrack in %s", msn_.movie.c_str());
}

// ---------------------------------------------------------------- movies
// Briefing movies are Shell\movies\*.avi (found through the data root); the full-screen ones are in the CD's
// Setup\movies (or an installed game's movies folder), next to the data folder.
std::string Shell::findMovie(const std::string& name, bool shellMovie) const {
    if (name.empty()) return {};
    if (shellMovie) return app_.fs.findLoose("shell/movies/" + name);
    std::string p = app_.fs.findLoose("movies/" + name);
    if (!p.empty()) return p;
    std::string root = app_.fs.root();
    while (!root.empty() && (root.back() == '/' || root.back() == '\\')) root.pop_back();
    size_t s = root.find_last_of("/\\");
    std::vector<std::string> dirs = {root + "/movies", root + "/Movies", root + "/MOVIES"};
    if (s != std::string::npos)
        for (const char* d : {"/movies", "/Movies", "/MOVIES"}) dirs.push_back(root.substr(0, s) + d);
    for (auto& d : dirs)
        for (const std::string& n : {name, lower(name), upper(name)}) {
            std::string f = d + "/" + n;
            if (SDL_GetPathInfo(f.c_str(), nullptr)) return f;
        }
    return {};
}

bool Shell::startMovie(const std::string& name, bool shellMovie, bool fullscreen) {
    stopMovie();
    std::string p = findMovie(name, shellMovie);
    if (p.empty()) {
        SDL_Log("shell: movie %s not found", name.c_str());
        return false;
    }
    auto m = std::make_unique<Movie>();
    if (!m->open(p)) {
        SDL_Log("shell: cannot play %s", p.c_str());
        return false;
    }
    movie_.m = std::move(m);
    movie_.shown = -1;
    movie_.fullscreen = fullscreen;
    // the interlaced filter weaves two fields: twice the frame's lines, the odd ones from the previous frame
    movie_.interlaced = fullscreen && app_.settings.movieFilter == 2;
    movie_.prev = Image{};
    std::pair<int, int> size{movie_.m->width(), movie_.m->height() * (movie_.interlaced ? 2 : 1)};
    auto it = movieTex_.find(size);
    if (it == movieTex_.end()) {
        Image black;
        black.w = size.first, black.h = size.second;
        black.rgba.assign(size_t(black.w) * black.h * 4, 0);
        for (size_t i = 3; i < black.rgba.size(); i += 4) black.rgba[i] = 255;
        it = movieTex_.emplace(size, app_.r.createTexture(black, false)).first;
    }
    movie_.tex = it->second;
    if (app_.audioOk && movie_.m->hasAudio()) {
        if (fullscreen) app_.audio.stopAll(), humOn_ = false;
        app_.audio.playPcm(movie_.m->audioSpec(), movie_.m->audio().data(), movie_.m->audio().size(), 1.f);
    }
    movie_.t0 = SDL_GetTicksNS();
    return true;
}

void Shell::stopMovie() {
    if (!movie_.m) return;
    if (app_.audioOk) app_.audio.flushDialog();
    movie_.m.reset();
}

bool Shell::updateMovie() {
    if (!movie_.m) return false;
    double t = double(SDL_GetTicksNS() - movie_.t0) * 1e-9;
    int idx = int(t * movie_.m->fps());
    if (idx >= movie_.m->frames()) {
        stopMovie();
        return false;
    }
    if (idx != movie_.shown && movie_.m->frame(idx, movie_.img)) {
        if (movie_.interlaced) {
            const Image& c = movie_.img;
            const Image& p = movie_.prev.w == c.w && movie_.prev.h == c.h ? movie_.prev : c;
            Image& o = movie_.woven;
            o.w = c.w, o.h = c.h * 2, o.hasAlpha = false;
            o.rgba.resize(size_t(o.w) * o.h * 4);
            size_t row = size_t(c.w) * 4;
            for (int y = 0; y < c.h; y++) {
                memcpy(&o.rgba[size_t(2 * y) * row], &c.rgba[size_t(y) * row], row);       // this frame's field
                memcpy(&o.rgba[size_t(2 * y + 1) * row], &p.rgba[size_t(y) * row], row);   // the last frame's field
            }
            app_.r.updateTexture(movie_.tex, o);
            movie_.prev = c;
        } else {
            app_.r.updateTexture(movie_.tex, movie_.img);
        }
        movie_.shown = idx;
    }
    return true;
}

bool Shell::fullscreenMovie() {
    if (!moviesEnabled_) movieQueue_.clear();
    while (!(movie_.m && movie_.fullscreen) && !movieQueue_.empty()) {
        std::string n = movieQueue_.front();
        movieQueue_.erase(movieQueue_.begin());
        startMovie(n, false, true);
    }
    if (!movie_.m || !movie_.fullscreen) return false;
    if (ui_.anyKeyPressed() || ui_.mousePressed()) {
        stopMovie();
        movieQueue_.clear();  // one skip ends the sequence
        return true;          // the key / click is used up by the skip
    }
    if (!updateMovie()) return !movieQueue_.empty();
    // The whole window in UI units, black. The picture (16:9, 320x180) goes either on a 4:3 screen in the middle,
    // letterboxed as the original showed it on a 640x480 display, or as large as the window allows.
    float W = float(app_.r.width()), H = float(app_.r.height()), s = ui_.scale();
    float ox = (W - 640.f * s) * 0.5f / s, oy = (H - 480.f * s) * 0.5f / s;
    float aw = W / s, ah = H / s;
    ui_.rect(-ox, -oy, aw, ah, 0x000000ffu);
    float sx = -ox, sy = -oy, sw = aw, sh = ah;  // the "screen"
    if (app_.settings.movie43) {
        sh = std::min(ah, aw * 0.75f), sw = sh * 4.f / 3.f;
        sx = -ox + (aw - sw) * 0.5f, sy = -oy + (ah - sh) * 0.5f;
    }
    float mw = float(movie_.m->width()), mh = float(movie_.m->height());
    float k = std::min(sw / mw, sh / mh);
    float w = mw * k, h = mh * k, mx = sx + (sw - w) * 0.5f, my = sy + (sh - h) * 0.5f;
    if (movie_.shown >= 0) ui_.textureQuad(movie_.tex, mx, my, w, h);
    int filter = app_.settings.movieFilter;
    if (filter && movie_.shown >= 0) {
        movieFilterTextures();
        if (filter == 1) {
            // CRT: a dark gap between the picture's source lines (one scanline per line of the 180), the screen's
            // edges and corners falling off
            float period = h / mh, v0 = -(my - sy) / period;
            ui_.textureQuadUV(scanTex_, sx, sy, sw, sh, 0, v0, 1, v0 + sh / period, 0xffffffffu);
            ui_.textureQuadUV(vignetteTex_, sx, sy, sw, sh, 0, 0, 1, 1, 0xffffffffu);
        } else {
            // interlaced: the two fields woven (above), the second field's lines a little darker
            float period = h / (mh * 2.f);
            ui_.textureQuadUV(fieldTex_, mx, my, w, h, 0, 0, 1, h / period, 0xffffffffu);
        }
    }
    return true;
}

void Shell::movieFilterTextures() {
    if (scanTex_ >= 0) return;
    Image sc;  // one scanline: 8 rows, bright in the middle, dark between lines (alpha = darkness)
    sc.w = 4, sc.h = 8, sc.hasAlpha = true;
    sc.rgba.assign(size_t(sc.w) * sc.h * 4, 0);
    for (int y = 0; y < sc.h; y++) {
        float t = std::sin(3.14159f * (float(y) + 0.5f) / float(sc.h));
        uint8_t a = uint8_t(255.f * 0.7f * std::pow(1.f - t, 1.4f));
        for (int x = 0; x < sc.w; x++) sc.rgba[(size_t(y) * sc.w + x) * 4 + 3] = a;
    }
    scanTex_ = app_.r.createTexture(sc, false);
    Image f;  // two field lines: the first clear, the second dimmed
    f.w = 4, f.h = 2, f.hasAlpha = true;
    f.rgba.assign(size_t(f.w) * f.h * 4, 0);
    for (int x = 0; x < f.w; x++) f.rgba[(size_t(1) * f.w + x) * 4 + 3] = 70;
    fieldTex_ = app_.r.createTexture(f, false);
    Image v;  // the tube's edges: darkening toward the sides and more into the corners
    v.w = 128, v.h = 96, v.hasAlpha = true;
    v.rgba.assign(size_t(v.w) * v.h * 4, 0);
    for (int y = 0; y < v.h; y++)
        for (int x = 0; x < v.w; x++) {
            float dx = (float(x) + 0.5f) / float(v.w) * 2.f - 1.f, dy = (float(y) + 0.5f) / float(v.h) * 2.f - 1.f;
            float e = std::pow(std::pow(std::fabs(dx), 6.f) + std::pow(std::fabs(dy), 6.f), 1.f / 6.f);  // rounded rect
            float a = std::clamp((e - 0.78f) / 0.22f, 0.f, 1.f);
            a = a * a * (3.f - 2.f * a) * 0.85f + 0.12f * (dx * dx + dy * dy) * 0.5f;
            v.rgba[(size_t(y) * v.w + x) * 4 + 3] = uint8_t(255.f * std::min(1.f, a));
        }
    vignetteTex_ = app_.r.createTexture(v, false);
}

// ---------------------------------------------------------------- briefing
void Shell::briefing() {
    background(kTrideoArt);
    // the user's gear
    ui_.text["pilot_0.sw.name"] = user_.pilot;
    ui_.text["pilot_0.sw.chassis"] = slotChassisName(user_);
    ui_.text["pilot_0.sw.variant"] = slotVariantName(user_);
    ui_.hidden["pilot_0.sw.threat"] = true;
    ui_.disabled["pilot_0.sw.chassis"] = true;
    std::vector<std::string> vn;
    std::vector<int> vi;
    if (user_.chassis >= 0 && size_t(user_.chassis) < data_.chassis.size() && msn_.forcedChassis.empty()) {
        const auto& v = data_.chassis[size_t(user_.chassis)].variants;
        for (size_t i = 0; i < v.size(); i++)
            if (!v[i].hidden) vn.push_back(v[i].name), vi.push_back(int(i));
    }
    ui_.disabled["pilot_0.sw.variant"] = vn.size() < 2;
    ui_.window("pilot_0");
    int vp = dropdown("pilot_0.sw.variant", vn, int(std::find(vi.begin(), vi.end(), user_.variant) - vi.begin()), "uservariant");
    if (vp >= 0) {
        user_.variant = vi[size_t(vp)], user_.custom = true;
        if (CampaignPilot* p = pilot(); p && mode_ == Mode::Campaign) p->variant = slotVariantName(user_);
    }
    bool squadScreen = mode_ != Mode::Training && msn_.squadMax > 1 && (!msn_.pilots.empty() || mode_ == Mode::Historical);
    if (msn_.squadMax > 1) ui_.window("away_squad_title");
    for (int i = 0; i < 4; i++) {
        std::string w = "pilot_" + std::to_string(i + 1);
        if (size_t(i) >= squad_.size()) continue;
        const Slot& s = squad_[size_t(i)];
        ui_.text[w + ".sw.name"] = s.pilot;
        ui_.text[w + ".sw.chassis"] = slotChassisName(s);
        ui_.text[w + ".sw.variant"] = slotVariantName(s);
        ui_.hidden[w + ".sw.threat"] = true;
        ui_.disabled[w + ".sw.variant"] = true;
        UiRect r = ui_.window(w);
        if (const Pilot* pp = data_.findPilot(s.pilot)) ui_.image(pp->portrait, r.x + 3, r.y + 3, 65, 58);
    }
    // SATCOM movie area: the still, with the narration on PLAY
    ui_.text["ln_brf_video_title.static"] = msn_.title.empty() || msn_.title == "TEMP TITLE" ? missionName(msn_) : msn_.title;
    ui_.window("ln_brf_video_title");
    ui_.hidden["ln_brf_video.display"] = true;
    UiRect vr = ui_.rectOf("ln_brf_video");
    ui_.image("SFRA016X.TGA", vr.x, vr.y);
    bool still = false;
    std::string num = msn_.id.size() >= 3 ? msn_.id.substr(1) : msn_.id;
    for (const std::string& img : {msn_.image, std::string("SSAVES") + num + ".TGA"}) {
        int iw, ih;
        if (!still && lower(img) != "sfra016x.tga" && ui_.imageSize(img, iw, ih)) ui_.image(img, vr.x, vr.y, 300, 130), still = true;
    }
    bool playing = movie_.m && !movie_.fullscreen && updateMovie();
    if (playing && movie_.shown >= 0) ui_.textureQuad(movie_.tex, vr.x, vr.y, 300, 130);
    bool speaking = playing || (app_.audioOk && app_.audio.dialogPlaying());
    bool hasMovie = data_.hasMovie(msn_.movie);
    if (!speaking && hasMovie) {
        TextStyle st = ui_.style(ui_.ldl().find("ln_brf_video.display"));
        st.vcenter = true, st.just = "center";
        ui_.rect(vr.x, vr.y + vr.h * 0.5f - 12, vr.w, 24, 0x00000090u);
        ui_.textBox("PRESS PLAY FOR SATCOM INTEL", {vr.x, vr.y, vr.w, vr.h}, st, st.color);
    }
    if (hasMovie) {
        ui_.selected["ln_brf_video_controller.play"] = speaking;
        ui_.window("ln_brf_video_controller");
        if (ui_.clicked("ln_brf_video_controller.play")) playBriefingAudio();
        if (ui_.clicked("ln_brf_video_controller.stop")) {
            stopMovie();
            if (app_.audioOk) app_.audio.flushDialog();
        }
    }
    ui_.window("ln_brf_summary_title");
    UiRect r = ui_.rectOf("ln_brf_summary");
    ui_.image("SDBX001X.TGA", r.x, r.y);
    TextStyle st;
    st.size = 7, st.wrap = true, st.insetX = 10, st.insetY = 7;
    ui_.scrollText(msn_.briefing, {r.x, r.y, 300, r.h}, st, briefScroll_);
    if (!threatDlg_.empty()) {
        std::string msg = ui_.text[threatDlg_ + ".pane.msg"];
        ui_.window(threatDlg_);
        if (!ui_.ldl().find(threatDlg_)) {  // no layout: a plain box
            ui_.rect(170, 190, 300, 90, 0x000000e0u);
            ui_.frame(170, 190, 300, 90, 0x4c8ef8ffu);
            TextStyle st;
            st.size = 8, st.wrap = true, st.insetX = 10, st.insetY = 10;
            ui_.textBox(msg + "\n\nPress Enter.", {170, 190, 300, 90}, st, 0xc0e0ffffu);
        }
        if (ui_.clicked(threatDlg_ + ".pane.ok") || ui_.keyPressed(SDLK_RETURN) || ui_.keyPressed(SDLK_ESCAPE) ||
            (!ui_.ldl().find(threatDlg_) && ui_.mouseReleased()))
            threatDlg_.clear();
        return;
    }
    // buttons
    ui_.window("back");
    bool gearOk = msn_.forcedChassis.empty();
    if (gearOk) ui_.window("low_gear");
    if (squadScreen) ui_.window("low_squad");
    ui_.window("low_launch");
    if (ui_.clicked("back.back") || ui_.keyPressed(SDLK_ESCAPE)) {
        if (app_.audioOk) app_.audio.flushDialog();
        if (mode_ == Mode::Campaign) go(Scr::Pilots);
        else go(Scr::Missions);
    } else if (ui_.clicked("low_gear.btn")) {
        gearSlot_ = 0, gearNotesScroll_ = 0;
        back_ = Scr::Briefing;
        go(Scr::Gear);
    } else if (ui_.clicked("low_squad.btn")) {
        go(Scr::Squad);
    } else if (ui_.clicked("low_launch.btn")) {
        // the mission's threat limits: threatMin is the most one gear may carry, threatMax the squad's total
        int total = 0, worst = 0;
        Threat t;
        if (slotThreat(user_, t)) total += t.total, worst = std::max(worst, t.total);
        for (auto& s : squad_)
            if (slotThreat(s, t)) total += t.total, worst = std::max(worst, t.total);
        if (msn_.threatMin > 0 && worst > msn_.threatMin) {
            threatDlg_ = "dlg_exceed_gear_max_threat";
            ui_.text[threatDlg_ + ".pane.msg"] = "A gear in your squad has a threat value of " + std::to_string(worst) +
                                                 ". This mission allows at most " + std::to_string(msn_.threatMin) + " per gear.";
        } else if (msn_.threatMax > 0 && total > msn_.threatMax) {
            threatDlg_ = "dlg_exceed_max_threat";
            ui_.text[threatDlg_ + ".pane.msg"] = "Your squad's threat value is " + std::to_string(total) + ". This mission allows " +
                                                 std::to_string(msn_.threatMax) + ".";
        } else {
            launch();
        }
    }
}

// ---------------------------------------------------------------- squad detail
void Shell::squad() {
    background(kTrideoArt);
    std::vector<std::string> pool;
    CampaignPilot* p = pilot();
    if (mode_ == Mode::Campaign) pool = msn_.pilots;
    else
        for (auto& pp : data_.pilots) pool.push_back(pp.name);
    if (pool.size() > 10) pool.resize(10);
    int room = std::max(0, msn_.squadMax - 1);
    for (size_t i = 0; i < pool.size(); i++) {
        std::string w = "r_pilot_" + std::to_string(i + 1);
        const Pilot* pp = data_.findPilot(pool[i]);
        bool dead = p && mode_ == Mode::Campaign && std::find(p->lost.begin(), p->lost.end(), pool[i]) != p->lost.end();
        auto it = std::find_if(squad_.begin(), squad_.end(), [&](const Slot& s) { return s.pilot == pool[i]; });
        Slot shown;
        if (it != squad_.end()) shown = *it;
        else if (pp) {
            const Chassis* c = data_.findChassis(pp->chassis);
            shown.chassis = c ? c->index : -1;
            shown.variant = defaultVariant(shown.chassis);
            if (c)
                for (size_t k = 0; k < c->variants.size(); k++)
                    if (lower(c->variants[k].name) == lower(pp->variant)) shown.variant = int(k);
        }
        ui_.text[w + ".sw.name"] = pool[i] + (dead ? " (KIA)" : "");
        ui_.text[w + ".sw.chassis"] = slotChassisName(shown);
        ui_.text[w + ".sw.variant"] = slotVariantName(shown);
        ui_.hidden[w + ".sw.threat"] = true;
        ui_.selected[w + ".sw"] = it != squad_.end();
        ui_.disabled[w + ".sw"] = dead;
        UiRect r = ui_.window(w);
        if (pp) ui_.image(pp->portrait, r.x + 3, r.y + 3, 65, 58, dead ? 0x606060ffu : 0xffffffffu);
        if (it != squad_.end()) {
            std::string n = "r_pilot_s_" + std::to_string(i + 1);
            ui_.text[n + ".sw"] = std::to_string(int(it - squad_.begin()) + 1);
            ui_.window(n);
        }
        if (ui_.clicked(w + ".sw") && !dead) {
            bioPilot_ = pool[i], bioScroll_ = 0;
            bool required = false;
            for (size_t k = 0; k < msn_.pilots.size(); k++)
                if (msn_.pilots[k] == pool[i] && k < msn_.pilotRequired.size() && msn_.pilotRequired[k]) required = true;
            if (it != squad_.end() && !required) squad_.erase(it);
            else if (it == squad_.end() && int(squad_.size()) < room) {
                Slot s = shown;
                s.pilot = pool[i];
                if (s.chassis < 0) s.chassis = 0;
                squad_.push_back(s);
            }
        }
    }
    // biography of the last pilot clicked
    if (bioPilot_.empty() && !squad_.empty()) bioPilot_ = squad_[0].pilot;
    ui_.window("pilot_bio");
    if (const Pilot* bp = data_.findPilot(bioPilot_)) {
        UiRect br = ui_.rectOf("pilot_bio.display");
        TextStyle st;
        st.size = 7, st.wrap = true, st.insetX = 6, st.insetY = 6;
        ui_.scrollText(bp->bio, {br.x, br.y, 123, br.h}, st, bioScroll_);
    }
    char buf[64];
    snprintf(buf, sizeof buf, "Squad mates: %d of %d", int(squad_.size()), room);
    ui_.text["squad_select_threat_total.body"] = buf;
    ui_.window("squad_select_threat_total");
    ui_.window("back");
    ui_.window("low_accept_squad");
    if (ui_.clicked("low_accept_squad.btn")) {
        if (p && mode_ == Mode::Campaign) {
            p->squad.clear();
            for (auto& s : squad_) p->squad.push_back(s.pilot);
        }
        go(Scr::Briefing);
    } else if (ui_.clicked("back.back") || ui_.keyPressed(SDLK_ESCAPE)) {
        go(Scr::Briefing);
    }
}

// ---------------------------------------------------------------- gear (chassis / variant)
void Shell::gear() {
    background(kTrideoArt);
    Slot* s = &user_;
    if (mode_ == Mode::Instant || mode_ == Mode::Multi) {
        if (gearSlot_ >= 100) s = &iaEnemies_[size_t(std::clamp(gearSlot_ - 100, 0, 4))];
        else s = &iaSquad_[size_t(std::clamp(gearSlot_, 0, 4))];
    } else if (gearSlot_ > 0 && size_t(gearSlot_ - 1) < squad_.size()) {
        s = &squad_[size_t(gearSlot_ - 1)];
    }
    // the campaign keeps the registered chassis (variants only); historical and instant action pick freely
    bool chassisFree = mode_ != Mode::Campaign;
    std::vector<std::string> names;
    std::vector<int> idx;
    if (mode_ == Mode::Historical || mode_ == Mode::Training) names.push_back("Mission Gear"), idx.push_back(-1);
    for (auto& c : data_.chassis) names.push_back(c.name), idx.push_back(c.index);
    int cur = int(std::find(idx.begin(), idx.end(), s->chassis) - idx.begin());
    ui_.window("gear_subtitle_chassis");
    ui_.text["gear_subtitle_chassis_btn.sw.btn"] = slotChassisName(*s);
    ui_.disabled["gear_subtitle_chassis_btn.sw.btn"] = !chassisFree;
    ui_.window("gear_subtitle_chassis_btn");
    int pick = dropdown("gear_subtitle_chassis_btn.sw.btn", names, cur, "chassis");
    if (pick >= 0 && chassisFree) {
        s->custom = true;
        s->chassis = idx[size_t(pick)];
        s->variant = defaultVariant(s->chassis);
        s->refit = false, s->weapons.clear(), s->restat = false;
        gearNotesScroll_ = 0;
    }
    // variant list (hidden training variants excluded)
    std::vector<std::string> vnames;
    std::vector<int> vidx;
    if (s->chassis >= 0 && size_t(s->chassis) < data_.chassis.size()) {
        const auto& v = data_.chassis[size_t(s->chassis)].variants;
        for (size_t i = 0; i < v.size(); i++)
            if (!v[i].hidden || int(i) == s->variant) vnames.push_back(v[i].name), vidx.push_back(int(i));
    }
    ui_.text["gear_title_variant.input"] = slotVariantName(*s);
    ui_.window("gear_title_variant");
    UiRect vin = ui_.rectOf("gear_title_variant.input");
    ui_.image("SBTN026U.TGA", vin.x, vin.y, vin.w, vin.h, 0xffffffffu, "stretch");
    TextStyle vst = ui_.style(ui_.ldl().find("gear_title_variant.input"));
    vst.insetX = 10, vst.vcenter = true;
    ui_.textBox(slotVariantName(*s), vin, vst, vst.color);
    if (ui_.mouseReleased() && vin.contains(ui_.mouseX(), ui_.mouseY()) && !vnames.empty() && !popup_.open) {
        popup_ = Popup{};
        popup_.id = "variant", popup_.anchor = vin, popup_.items = vnames, popup_.open = true;
        popup_.sel = int(std::find(vidx.begin(), vidx.end(), s->variant) - vidx.begin());
        ui_.playSound("general.wav");
    }
    if (popup_.picked >= 0 && popup_.id == "variant") {
        s->custom = true;
        s->variant = vidx[size_t(popup_.picked)];
        s->refit = false, s->weapons.clear(), s->restat = false;
        popup_ = Popup{};
    }
    // picture, weapons, description
    const Chassis* ch = s->chassis >= 0 && size_t(s->chassis) < data_.chassis.size() ? &data_.chassis[size_t(s->chassis)] : nullptr;
    ui_.window("gear_image");
    if (ch) ui_.image(ch->picture, 79, 97, 188, 220);
    // weapons: click a mount to refit it with any weapon stock gears carry on that hardpoint
    auto loadout = loadoutFor(*s);
    auto wname = [&](int wi) {
        std::string w = wi >= 0 && size_t(wi) < data_.weaponFiles.size() ? data_.weaponFiles[size_t(wi)] : "?";
        return w.substr(0, w.find('.'));
    };
    {
        UiRect hr = ui_.rectOf("gear_hardpoints");
        TextStyle hs;
        hs.font = "eub_____.ttf", hs.size = 8, hs.color = 0x4c8ef8ffu;
        ui_.textBox(s->refit ? "WEAPONS (REFIT)" : "WEAPONS", {hr.x + 10, hr.y, 150, 20}, hs, hs.color);
        TextStyle ws;
        ws.size = 7, ws.color = 0x8cc8ffffu;
        float y = hr.y + 22;
        if (loadout.empty()) ui_.textBox(ch ? "Standard loadout" : "The mission's own gear", {hr.x + 6, y, 160, 14}, ws, ws.color);
        bool canRefit = ch != nullptr;
        for (size_t k = 0; k < loadout.size(); k++) {
            UiRect er{hr.x + 3, y, 150, 16};
            bool hot = canRefit && er.contains(ui_.mouseX(), ui_.mouseY()) && !popup_.open;
            ui_.image("SBTN025U.TGA", er.x, er.y, er.w, er.h, hot ? 0xc0e0ffffu : 0xffffffffu, "stretch");
            TextStyle w2 = ws;
            w2.vcenter = true, w2.insetX = 4;
            ui_.textBox("HP" + std::to_string(loadout[k].first) + "  " + wname(loadout[k].second), er, w2, hot ? 0xffffffffu : ws.color);
            if (hot && ui_.mouseReleased()) {
                std::vector<int> cand = weaponsForHardpoint(loadout[k].first);
                VarRecord erec;
                if (slotRecord(*s, erec) && (erec.perks >> 8 & 1)) {  // Easy To Modify: anything stock gears mount anywhere
                    std::set<int> all;
                    for (int hp = 1; hp <= 9; hp++)
                        for (int w : weaponsForHardpoint(hp)) all.insert(w);
                    cand.assign(all.begin(), all.end());
                }
                if (std::find(cand.begin(), cand.end(), loadout[k].second) == cand.end()) cand.insert(cand.begin(), loadout[k].second);
                popup_ = Popup{};
                popup_.id = "wpn" + std::to_string(k), popup_.anchor = er, popup_.open = true;
                for (int wi : cand) popup_.items.push_back(wname(wi));
                popup_.items.push_back("(remove)");
                popup_.sel = int(std::find(cand.begin(), cand.end(), loadout[k].second) - cand.begin());
                refitChoices_ = cand;
                ui_.playSound("general.wav");
            }
            y += 20;
        }
        if (popup_.picked >= 0 && popup_.id.rfind("wpn", 0) == 0) {
            size_t k = size_t(atoi(popup_.id.c_str() + 3));
            if (!s->refit) s->weapons = loadout, s->refit = true;
            if (k < s->weapons.size()) {
                if (size_t(popup_.picked) < refitChoices_.size()) s->weapons[k].second = refitChoices_[size_t(popup_.picked)];
                else s->weapons.erase(s->weapons.begin() + long(k));
            }
            s->custom = true;
            popup_ = Popup{};
        }
        if (s->refit) {  // back to the variant's own weapons
            UiRect rr{hr.x + 3, y + 4, 150, 16};
            bool hot = rr.contains(ui_.mouseX(), ui_.mouseY()) && !popup_.open;
            TextStyle w2 = ws;
            w2.vcenter = true, w2.insetX = 4;
            ui_.image("SBTN025U.TGA", rr.x, rr.y, rr.w, rr.h, hot ? 0xc0e0ffffu : 0xa0a0a0ffu, "stretch");
            ui_.textBox("RESTORE STANDARD WEAPONS", rr, w2, hot ? 0xffffffffu : ws.color);
            if (hot && ui_.mouseReleased()) s->refit = false, s->weapons.clear(), ui_.playSound("general.wav");
        }
    }
    if (ch) {
        // statistics from the master gear definition
        Bytes g;
        GearDef d;
        if (masterGear(app_.fs, ch->index, g) && parseGearDef(g.data(), g.size(), d)) {
            static const char* cls[] = {"", "Light", "Medium", "Heavy", "Assault"};
            VarRecord vr, base;
            bool rec = slotRecord(*s, vr);
            if (rec) d.armor = vr.armor, d.sensors = vr.sensors, d.fireControl = vr.fireControl, d.maneuverability = vr.maneuver;
            bool haveBase = s->variant >= 0 && size_t(s->variant) < ch->variants.size() && data_.variantRecord(ch->variants[size_t(s->variant)].file, base);
            ui_.image("SDBX003X.TGA", 470, 97);
            TextStyle hs;
            hs.font = "eub_____.ttf", hs.size = 8, hs.color = 0x4c8ef8ffu, hs.just = "center";
            ui_.textBox(s->restat ? "STRUCTURE*" : "STRUCTURE", {470, 103, 88, 16}, hs, hs.color);
            TextStyle ss;
            ss.size = 7, ss.color = 0x8cc8ffffu;
            char buf[64];
            float y = 124;
            auto line = [&](const std::string& t) {
                ui_.textBox(t, {476, y, 82, 12}, ss, ss.color);
                y += 13;
            };
            line(std::string("Class: ") + cls[std::clamp(ch->weightClass, 0, 4)]);
            // structure ratings with - / + (gear lab structure page): ranges from SNSRRTBL / FCRATTBL / MNVRTABL;
            // armour within 5 of the variant's own
            struct Stat { const char* name; int* v; int lo, hi; bool sign; };
            Stat stats[] = {{"Armor", &vr.armor, std::max(1, base.armor - 5), base.armor + 5, false},
                            {"Sensors", &vr.sensors, -5, 5, true},
                            {"Fire ctl", &vr.fireControl, -5, 3, true},
                            {"Maneuver", &vr.maneuver, -3, 3, true}};
            for (auto& st : stats) {
                snprintf(buf, sizeof buf, st.sign ? "%s: %+d" : "%s: %d", st.name, *st.v);
                ui_.textBox(buf, {476, y, 60, 12}, ss, ss.color);
                if (rec && haveBase && mode_ != Mode::Training) {
                    for (int k = 0; k < 2; k++) {
                        UiRect br{536.f + float(k) * 11.f, y, 10, 11};
                        bool hot = br.contains(ui_.mouseX(), ui_.mouseY()) && !popup_.open;
                        ui_.rect(br.x, br.y, br.w, br.h, hot ? 0x2a64c0ffu : 0x10306080u);
                        TextStyle bs = ss;
                        bs.just = "center", bs.vcenter = true;
                        ui_.textBox(k ? "+" : "-", br, bs, 0xffffffffu);
                        if (hot && ui_.mouseReleased()) {
                            int nv = std::clamp(*st.v + (k ? 1 : -1), st.lo, st.hi);
                            if (nv != *st.v) {
                                if (!s->restat) s->rec = base, s->restat = true;
                                *st.v = nv;
                                s->rec.armor = vr.armor, s->rec.sensors = vr.sensors, s->rec.fireControl = vr.fireControl,
                                s->rec.maneuver = vr.maneuver, s->rec.perks = vr.perks, s->rec.flaws = vr.flaws;
                                s->custom = true;
                                ui_.playSound("general.wav");
                            }
                        }
                    }
                }
                y += 13;
            }
            snprintf(buf, sizeof buf, "Walk: %.0f km/h", d.maxWalkSpeed * 3.6f);
            line(buf);
            snprintf(buf, sizeof buf, "Run: %.0f km/h", d.maxRunSpeed * 3.6f);
            line(buf);
            // perks & flaws page: a list of every perk and flaw; picking one cycles its level
            if (rec && haveBase && mode_ != Mode::Training) {
                y += 6;
                UiRect pr{474, y, 80, 14};
                bool hot = pr.contains(ui_.mouseX(), ui_.mouseY()) && !popup_.open;
                ui_.image("SBTN025U.TGA", pr.x, pr.y, pr.w, pr.h, hot ? 0xc0e0ffffu : 0xffffffffu, "stretch");
                TextStyle bs = ss;
                bs.just = "center", bs.vcenter = true;
                ui_.textBox("PERKS & FLAWS", pr, bs, hot ? 0xffffffffu : ss.color);
                if (hot && ui_.mouseReleased()) {
                    popup_ = Popup{};
                    popup_.id = "perk", popup_.anchor = pr, popup_.open = true;
                    perkChoices_.clear();
                    const auto& fl = GameData::perkFields();
                    for (size_t i = 0; i < fl.size(); i++) {
                        uint32_t w = fl[i].word ? vr.flaws : vr.perks;
                        int lv = int(w >> fl[i].shift & ((1u << fl[i].width) - 1));
                        std::string label = (fl[i].id >= 35 ? "[F] " : "[P] ") + data_.perkLabel(fl[i].id);
                        if (fl[i].width > 1) label += "  " + std::to_string(lv) + "/" + std::to_string((1 << fl[i].width) - 1);
                        else label += lv ? "  ON" : "  -";
                        popup_.items.push_back(label);
                        perkChoices_.push_back(int(i));
                    }
                    ui_.playSound("general.wav");
                }
                y += 18;
                if (s->restat) {
                    UiRect rr{474, y, 80, 14};
                    bool h2 = rr.contains(ui_.mouseX(), ui_.mouseY()) && !popup_.open;
                    ui_.image("SBTN025U.TGA", rr.x, rr.y, rr.w, rr.h, h2 ? 0xc0e0ffffu : 0xa0a0a0ffu, "stretch");
                    ui_.textBox("RESTORE", rr, bs, h2 ? 0xffffffffu : ss.color);
                    if (h2 && ui_.mouseReleased()) s->restat = false, ui_.playSound("general.wav");
                }
            }
            if (popup_.picked >= 0 && popup_.id == "perk" && rec) {
                const auto& fl = GameData::perkFields();
                size_t i = size_t(perkChoices_[size_t(popup_.picked)]);
                if (!s->restat) s->rec = vr, s->restat = true;
                uint32_t& w = fl[i].word ? s->rec.flaws : s->rec.perks;
                uint32_t mask = ((1u << fl[i].width) - 1) << fl[i].shift;
                uint32_t lv = ((w & mask) >> fl[i].shift) + 1;
                if (lv > ((1u << fl[i].width) - 1)) lv = 0;
                w = (w & ~mask) | (lv << fl[i].shift);
                s->custom = true;
                int keep = popup_.scroll;
                popup_ = Popup{};
                // reopen at the same place so several can be changed in a row
                popup_.id = "perk", popup_.anchor = {474, y - 18 - (s->restat ? 0.f : 0.f), 80, 14}, popup_.open = true, popup_.scroll = keep;
                VarRecord nr;
                slotRecord(*s, nr);
                for (size_t k = 0; k < fl.size(); k++) {
                    uint32_t ww = fl[k].word ? nr.flaws : nr.perks;
                    int l2 = int(ww >> fl[k].shift & ((1u << fl[k].width) - 1));
                    std::string label = (fl[k].id >= 35 ? "[F] " : "[P] ") + data_.perkLabel(fl[k].id);
                    if (fl[k].width > 1) label += "  " + std::to_string(l2) + "/" + std::to_string((1 << fl[k].width) - 1);
                    else label += l2 ? "  ON" : "  -";
                    popup_.items.push_back(label);
                }
                popup_.sel = int(i);
            }
        }
        ui_.window("gear_notes_general_title");
        UiRect nr = ui_.rectOf("gear_notes_general");
        ui_.image("SDBX011X.TGA", nr.x, nr.y);
        TextStyle st;
        st.size = 7, st.wrap = true, st.insetX = 6, st.insetY = 4;
        std::string notes = ch->description;
        VarRecord pr;
        if (slotRecord(*s, pr)) {
            std::vector<std::string> pk, fl;
            data_.perksAndFlaws(pr, pk, fl);
            if (!pk.empty()) notes += "\n\nPERKS: " + join(pk, ',');
            if (!fl.empty()) notes += "\n\nFLAWS: " + join(fl, ',');
        }
        ui_.scrollText(notes, {nr.x, nr.y, 174, nr.h}, st, gearNotesScroll_);
    }
    {
        Threat t;
        if (slotThreat(*s, t)) {
            ui_.text["gear_threat.total_val"] = std::to_string(t.total);
            ui_.text["gear_threat.offensive_val"] = std::to_string(t.offensive);
            ui_.text["gear_threat.defensive_val"] = std::to_string(t.defensive);
            ui_.text["gear_threat.misc_val"] = std::to_string(t.misc);
            ui_.window("gear_threat");
            if (msn_.threatMin > 0 && mode_ != Mode::Instant && t.total > msn_.threatMin) {  // over the mission's per-gear limit
                UiRect tr = ui_.rectOf("gear_threat");
                TextStyle ws;
                ws.size = 7, ws.color = 0xff6060ffu;
                ui_.textBox("Over the mission limit of " + std::to_string(msn_.threatMin), {tr.x, tr.y + tr.h + 2, 200, 14}, ws, ws.color);
            }
        }
    }
    ui_.window("back");
    ui_.window("low_accept_gear");
    if (ui_.clicked("low_accept_gear.btn") || ui_.clicked("back.back") || ui_.keyPressed(SDLK_ESCAPE)) {
        if (mode_ == Mode::Campaign && gearSlot_ == 0)
            if (CampaignPilot* p = pilot()) p->variant = slotVariantName(user_), p->refit = refitString(user_), p->restat = restatString(user_);
        go(back_ == Scr::Instant ? Scr::Instant : back_ == Scr::Multi ? Scr::Multi : Scr::Briefing);
    }
}

// ---------------------------------------------------------------- launch / debrief
void Shell::launch() {
    MissionSetup setup;
    setup.mission = msn_.id;
    if (mode_ == Mode::Instant) {
        setup.mission = data_.iaMaps.empty() ? "D01" : data_.iaMaps[size_t(std::clamp(iaMap_, 0, int(data_.iaMaps.size()) - 1))];
        setup.player.chassis = iaSquad_[0].chassis;
        setup.player = gearChoice(iaSquad_[0]);
        for (size_t i = 1; i < iaSquad_.size(); i++)
            if (iaSquad_[i].used) setup.squad.push_back({iaSquad_[i].pilot, gearChoice(iaSquad_[i])});
        for (auto& e : iaEnemies_)
            if (e.used) setup.enemies.push_back({e.pilot, gearChoice(e)});
        if (setup.enemies.empty()) setup.enemies.push_back({iaEnemies_[0].pilot, gearChoice(iaEnemies_[0])});
        setup.waves = std::max(1, iaWaves_);
    } else {
        if (mode_ == Mode::Campaign || user_.custom || !msn_.forcedChassis.empty()) {
            setup.player = gearChoice(user_);
        }
        for (auto& s : squad_) setup.squad.push_back({s.pilot, gearChoice(s)});
        if (mode_ == Mode::Campaign) autosave('L');
    }
    if (CampaignPilot* p = pilot(); p && mode_ == Mode::Campaign) setup.difficulty = p->difficulty;
    if (mode_ != Mode::Instant) setup.space = msn_.space;
    if (app_.audioOk) app_.audio.stopAll();
    SDL_Log("launch %s: chassis %d, %zu squad mates, %zu enemies x %d waves", setup.mission.c_str(), setup.player.chassis, setup.squad.size(),
            setup.enemies.size(), setup.waves);
    ui_.end();  // drop this frame's overlay
    int firstTex = app_.r.textureCount();
    result_ = runMission(app_, setup);
    humOn_ = false;  // the mission stopped the shell's loops
    ui_.forgetTextures(firstTex);
    haveResult_ = true;
    if (result_.outcome == MissionResult::Quit || app_.quit) {
        quit_ = true;
        return;
    }
    if (result_.outcome == MissionResult::Error) SDL_Log("mission %s failed to load: %s", setup.mission.c_str(), result_.error.c_str());
    if (CampaignPilot* p = pilot(); p && mode_ == Mode::Campaign && result_.outcome != MissionResult::Error) {
        p->sorties++;
        p->kills += result_.kills, p->shots += result_.shots, p->hits += result_.hits;
        if (result_.outcome == MissionResult::Failure) p->deaths++;
        if (result_.outcome == MissionResult::Success || result_.outcome == MissionResult::Alternate)
            for (auto& l : result_.squadLost)
                if (std::find(p->lost.begin(), p->lost.end(), l) == p->lost.end()) p->lost.push_back(l);
        saveRoster();
    }
    go(Scr::Debrief);
    ui_.begin(app_.r.width(), app_.r.height());
}

void Shell::debrief() {
    background(kTrideoArt);
    const MissionResult& r = result_;
    std::string text;
    bool won = r.outcome == MissionResult::Success, alt = r.outcome == MissionResult::Alternate;
    if (mode_ == Mode::Multi) {
        text = (won ? "You won the match.\n\n" : r.outcome == MissionResult::Aborted ? "You left the match.\n\n" : "Match over.\n\n") + mpScores_;
    } else if (mode_ == Mode::Instant) {
        text = won ? "All enemy waves destroyed." : r.outcome == MissionResult::Aborted ? "Mission aborted." : "Your squad was defeated.";
    } else {
        text = won ? msn_.success : alt ? msn_.alternate : msn_.failure;
        if (r.outcome == MissionResult::Aborted) text = "Mission aborted.";
        if (r.outcome == MissionResult::Error) text = "The mission could not be loaded:\n" + r.error;
    }
    if (!r.squadLost.empty()) text += "\n\nLost in action: " + join(r.squadLost, ',');
    {
        TextStyle ts = ui_.style(ui_.ldl().find("dbrf_title.title"));
        ts.vcenter = false;
        UiRect tr = ui_.rectOf("dbrf_title");
        std::string head = won ? "MISSION SUCCESSFUL" : alt ? "DEBRIEFING" : r.outcome == MissionResult::Aborted ? "MISSION ABORTED" : "MISSION FAILED";
        ui_.textBox(head, {tr.x, tr.y - 4, 300, 18}, ts, ts.color);
    }
    UiRect br = ui_.rectOf("dbrf_body");
    ui_.image("SDBX020X.TGA", br.x, br.y);
    TextStyle st;
    st.size = 7, st.wrap = true, st.insetX = 10, st.insetY = 8;
    ui_.scrollText(text, {br.x, br.y, 300, br.h}, st, briefScroll_);
    char buf[32];
    ui_.text["dbrf_stats.bg.kills_val"] = std::to_string(r.kills);
    snprintf(buf, sizeof buf, "%d%%", r.shots ? int(100.f * float(r.hits) / float(r.shots)) : 0);
    ui_.text["dbrf_stats.bg.acc_val"] = std::min(r.hits, r.shots) == r.hits ? buf : "100%";
    ui_.text["dbrf_stats.bg.cas_val"] = std::to_string(r.squadLost.size());
    ui_.text["dbrf_stats.bg.inj_val"] = hhmmss(r.time);
    ui_.text["dbrf_stats.bg.injuries"] = "TIME";
    ui_.window("dbrf_stats");
    ui_.window("low_dbrf_main");
    bool progress = mode_ == Mode::Campaign && (won || alt);
    if (progress) ui_.window("low_dbrf_continue");
    else ui_.window("low_dbrf_replay");
    if (ui_.clicked("low_dbrf_main.btn")) {
        go(mode_ == Mode::Campaign ? Scr::Pilots : mode_ == Mode::Instant ? Scr::Instant : mode_ == Mode::Multi ? Scr::Multi : Scr::Missions);
    } else if (ui_.clicked("low_dbrf_replay.btn")) {
        if (mode_ == Mode::Instant) launch();
        else if (mode_ == Mode::Multi) go(Scr::Multi);
        else go(Scr::Briefing);
    } else if (ui_.clicked("low_dbrf_continue.btn") && progress) {
        CampaignPilot* p = pilot();
        std::string next = won ? msn_.nextId : msn_.altId;
        if (next.empty()) next = msn_.nextId;
        autosave('R');
        if (lower(next) == "end" || next.empty()) {
            if (won) queueMovie(msn_.outroMovie);
            if (p) p->mission = kCampaignStart;
            saveRoster();
            creditScroll_ = 0, creditT_ = 0;
            go(Scr::Credits);
        } else {
            if (p) p->mission = next;
            saveRoster();
            if (won) queueMovie(msn_.outroMovie);
            if (openMission(next)) {
                go(Scr::Briefing);
                queueMovie(msn_.introMovie);
            }
        }
    }
}


// ---------------------------------------------------------------- multiplayer (the port's own LAN / internet play)
NetGear Shell::mpGear() const {
    GearChoice g = gearChoice(iaSquad_[0]);
    NetGear n;
    n.chassis = g.chassis, n.weapons = g.weapons, n.armor = g.armor, n.fireControl = g.fireControl;
    return n;
}

void Shell::multiplayer() {
    background(kTacticalArt);
    title("MULTIPLAYER");
    if (mpName_.empty()) mpName_ = pilot() ? pilot()->name : "Commander";
    TextStyle st, hs, ss;
    st.size = 8, st.insetX = 6, st.vcenter = true;
    hs.font = "eub_____.ttf", hs.size = 8, hs.color = 0x4c8ef8ffu;
    ss.size = 7, ss.color = 0x8cc8ffffu, ss.wrap = true;
    auto button = [&](const std::string& label, UiRect r, bool enabled = true) {
        bool hot = enabled && r.contains(ui_.mouseX(), ui_.mouseY()) && !popup_.open;
        ui_.image("SBTN025U.TGA", r.x, r.y, r.w, r.h, !enabled ? 0x606060ffu : hot ? 0xc0e0ffffu : 0xffffffffu, "stretch");
        TextStyle b = st;
        b.font = "eub_____.ttf", b.just = "center", b.insetX = 0;
        ui_.textBox(label, r, b, !enabled ? 0x6080a0ffu : hot ? 0xffffffffu : 0x8cc8ffffu);
        if (hot && ui_.mouseReleased()) {
            ui_.playSound("general.wav");
            return true;
        }
        return false;
    };
    auto field = [&](const std::string& label, std::string& value, UiRect r, int id, bool enabled) {
        ui_.textBox(label, {r.x, r.y - 14, r.w, 12}, hs, hs.color);
        ui_.rect(r.x, r.y, r.w, r.h, 0x00081890u);
        ui_.frame(r.x, r.y, r.w, r.h, mpField_ == id && enabled ? 0x8cc8ffffu : 0x3a6eb0ffu);
        if (enabled && ui_.mouseReleased() && r.contains(ui_.mouseX(), ui_.mouseY())) mpField_ = id;
        if (enabled) ui_.textField(value, r, st, 40, mpField_ == id);
        else ui_.textBox(value, r, st, 0x8cc8ffffu);
    };
    bool idle = net_.phase() == NetGame::Phase::Idle;
    if (!idle && !net_.poll()) {
        std::string why = net_.status;
        net_.stop();
        net_.status = why;
    }
    idle = net_.phase() == NetGame::Phase::Idle;
    // left: who you are and where to connect
    field("PILOT NAME", mpName_, {40, 90, 220, 18}, 0, idle);
    field("HOST ADDRESS (JOIN)", mpAddr_, {40, 135, 220, 18}, 1, idle);
    {
        Slot& me = iaSquad_[0];
        ui_.textBox("YOUR GEAR", {40, 168, 220, 12}, hs, hs.color);
        Threat t;
        std::string g = slotChassisName(me) + " / " + slotVariantName(me);
        if (slotThreat(me, t)) g += "  (threat " + std::to_string(t.total) + ")";
        ui_.textBox(g, {40, 182, 220, 14}, st, 0x8cc8ffffu);
        if (button("MODIFY GEAR", {40, 200, 140, 18}, idle)) gearSlot_ = 0, gearNotesScroll_ = 0, back_ = Scr::Multi, go(Scr::Gear);
    }
    if (idle) {
        if (button("HOST GAME", {40, 240, 140, 20})) {
            if (net_.host(mpName_, mpGear())) {
                if (!data_.iaMaps.empty()) net_.arena = data_.iaMaps[size_t(std::clamp(mpArena_, 0, int(data_.iaMaps.size()) - 1))];
                net_.fragLimit = mpFrags_, net_.timeLimit = mpMinutes_ * 60;
            }
        }
        if (button("JOIN GAME", {40, 266, 140, 20})) net_.join(mpAddr_, mpName_, mpGear());
    } else if (button("LEAVE", {40, 240, 140, 20})) {
        net_.stop();
        net_.status = "Left the game.";
    }
    // match settings (the host's; joined players see them)
    {
        float x = 300, y = 90;
        ui_.textBox("ARENA", {x, y - 14, 200, 12}, hs, hs.color);
        int arena = mpArena_;
        if (!net_.isHost() && !idle)
            for (size_t i = 0; i < data_.iaMaps.size(); i++)
                if (data_.iaMaps[i] == net_.arena) arena = int(i);
        std::string an = arena >= 0 && size_t(arena) < data_.iaNames.size() ? data_.iaNames[size_t(arena)] : net_.arena;
        UiRect ar{x, y, 220, 18};
        bool canSet = idle || net_.isHost();
        if (button(an, ar, canSet) && !data_.iaMaps.empty()) {
            popup_ = Popup{};
            popup_.id = "mparena", popup_.anchor = ar, popup_.open = true, popup_.sel = arena;
            for (size_t i = 0; i < data_.iaMaps.size(); i++) popup_.items.push_back(i < data_.iaNames.size() ? data_.iaNames[i] : data_.iaMaps[i]);
        }
        if (popup_.picked >= 0 && popup_.id == "mparena") {
            mpArena_ = popup_.picked;
            if (net_.isHost()) net_.arena = data_.iaMaps[size_t(mpArena_)];
            popup_ = Popup{};
        }
        char b[64];
        int frags = net_.isHost() || idle ? mpFrags_ : net_.fragLimit, mins = net_.isHost() || idle ? mpMinutes_ : net_.timeLimit / 60;
        snprintf(b, sizeof b, "FRAG LIMIT: %d", frags);
        ui_.textBox(b, {x, y + 26, 140, 18}, st, 0x8cc8ffffu);
        snprintf(b, sizeof b, "TIME LIMIT: %d MIN", mins);
        ui_.textBox(b, {x, y + 48, 140, 18}, st, 0x8cc8ffffu);
        if (canSet) {
            if (button("-", {x + 150, y + 26, 20, 18})) mpFrags_ = std::max(1, mpFrags_ - 1);
            if (button("+", {x + 174, y + 26, 20, 18})) mpFrags_ = std::min(50, mpFrags_ + 1);
            if (button("-", {x + 150, y + 48, 20, 18})) mpMinutes_ = std::max(1, mpMinutes_ - 1);
            if (button("+", {x + 174, y + 48, 20, 18})) mpMinutes_ = std::min(60, mpMinutes_ + 1);
            if (net_.isHost()) net_.fragLimit = mpFrags_, net_.timeLimit = mpMinutes_ * 60;
        }
        // players
        y += 86;
        ui_.textBox("PLAYERS", {x, y, 200, 12}, hs, hs.color);
        y += 16;
        ui_.rect(x, y, 300, 140, 0x00081870u);
        ui_.frame(x, y, 300, 140, 0x3a6eb0ffu);
        float py = y + 4;
        for (auto& p : net_.players()) {
            std::string chn = p.gear.chassis >= 0 && size_t(p.gear.chassis) < data_.chassis.size() ? data_.chassis[size_t(p.gear.chassis)].name : "?";
            ui_.textBox((p.id == 0 ? "[HOST] " : "") + p.name + "  -  " + chn, {x + 4, py, 292, 15}, st,
                        p.id == net_.myId() ? 0xffffffffu : 0x8cc8ffffu);
            py += 16;
        }
        if (net_.players().empty()) ui_.textBox(idle ? "Host a game, or join one by address." : "Waiting...", {x + 4, py, 292, 15}, st, 0x6080a0ffu);
        ui_.textBox(net_.status, {x, y + 146, 300, 40}, ss, ss.color);
        ui_.textBox("The port's own network game: players need the same game data. The host relays everything on UDP port "
                    "47250 (forward it for play over the internet).",
                    {40, 300, 230, 80}, ss, 0x6080a0ffu);
    }
    if (net_.isHost() && button("START MATCH", {460, 420, 150, 24}, net_.players().size() >= 2)) net_.start();
    if (button("BACK", {30, 420, 120, 24}) || (ui_.keyPressed(SDLK_ESCAPE) && !popup_.open)) {
        net_.stop();
        mode_ = Mode::Campaign;
        go(Scr::Main);
        return;
    }
    if (ui_.keyPressed(SDLK_TAB)) mpField_ = 1 - mpField_;
    if (net_.startReceived && net_.phase() == NetGame::Phase::Playing) {
        MissionSetup setup;
        setup.mission = net_.arena;
        setup.player = gearChoice(iaSquad_[0]);
        setup.net = &net_;
        ui_.end();
        int firstTex = app_.r.textureCount();
        SDL_Log("network match on %s with %zu players", setup.mission.c_str(), net_.players().size());
        result_ = runMission(app_, setup);
        humOn_ = false;
        ui_.forgetTextures(firstTex);
        haveResult_ = true;
        std::string scores;
        std::vector<NetPlayer> ps = net_.players();
        std::sort(ps.begin(), ps.end(), [](const NetPlayer& a, const NetPlayer& b) { return a.kills > b.kills; });
        for (auto& p : ps) scores += p.name + ": " + std::to_string(p.kills) + " kills, " + std::to_string(p.deaths) + " deaths\n";
        mpScores_ = scores;
        net_.stop();
        net_.status = "Match over.";
        if (result_.outcome == MissionResult::Quit || app_.quit) {
            quit_ = true;
            return;
        }
        go(Scr::Debrief);
        ui_.begin(app_.r.width(), app_.r.height());
    }
}

// ---------------------------------------------------------------- instant action
void Shell::instantAction() {
    background(kTacticalArt);
    auto chassisList = [&]() {
        std::vector<std::string> v;
        for (auto& c : data_.chassis) v.push_back(c.name);
        return v;
    };
    auto drawSlot = [&](const std::string& w, const std::string& sw, Slot& s, int gearId, bool toggleable) {
        ui_.text[w + ".sw.name"] = s.pilot.empty() ? "Commander" : s.pilot;
        ui_.text[w + ".sw.chassis"] = slotChassisName(s);
        ui_.text[w + ".sw.variant"] = slotVariantName(s);
        ui_.hidden[w + ".sw.threat"] = true;
        ui_.disabled[w + ".sw.chassis"] = !s.used;
        ui_.disabled[w + ".sw.variant"] = !s.used;
        ui_.window(w);
        if (!sw.empty()) {
            ui_.selected[sw + ".sw"] = s.used;
            ui_.text[sw + ".sw"] = s.used ? "ON" : "";
            ui_.window(sw);
            if (toggleable && ui_.clicked(sw + ".sw")) s.used = !s.used;
        }
        if (!s.used) return;
        int pick = dropdown(w + ".sw.chassis", chassisList(), s.chassis, "c" + w);
        if (pick >= 0) s.chassis = pick, s.variant = defaultVariant(pick);
        std::vector<std::string> vn;
        std::vector<int> vi;
        if (s.chassis >= 0 && size_t(s.chassis) < data_.chassis.size()) {
            const auto& v = data_.chassis[size_t(s.chassis)].variants;
            for (size_t i = 0; i < v.size(); i++)
                if (!v[i].hidden) vn.push_back(v[i].name), vi.push_back(int(i));
        }
        int vs = int(std::find(vi.begin(), vi.end(), s.variant) - vi.begin());
        int vp = dropdown(w + ".sw.variant", vn, vs, "v" + w);
        if (vp >= 0) s.variant = vi[size_t(vp)];
        if (ui_.clicked(w + ".sw") && !ui_.clicked(w + ".sw.chassis") && !ui_.clicked(w + ".sw.variant")) {
            gearSlot_ = gearId, gearNotesScroll_ = 0, back_ = Scr::Instant;
        }
    };
    ui_.window("ia_player_squad_header");
    ui_.window("ia_enemy_fireteams_header");
    for (int i = 0; i < 5; i++)
        drawSlot("ia_pilot_" + std::to_string(i), i == 0 ? std::string() : "ia_pilot_s" + std::to_string(i), iaSquad_[size_t(i)], i, i > 0);
    for (int i = 0; i < 5; i++)
        drawSlot("ia_enemy_" + std::to_string(i + 1), "ia_enemy_s" + std::to_string(i + 1), iaEnemies_[size_t(i)], 100 + i, i > 0);
    iaEnemies_[0].used = true, iaSquad_[0].used = true;
    // arena and waves
    ui_.window("ia_terrain");
    std::vector<std::string> maps = data_.iaNames;
    if (maps.size() > data_.iaMaps.size()) maps.resize(data_.iaMaps.size());
    int m = ui_.listbox("ia_terrain.lb", maps, iaMap_, listScroll_, "ia_strctr_lb_item");
    if (m >= 0) iaMap_ = m;
    ui_.hidden["ia_fire_teams.ranger"] = true;
    ui_.text["ia_fire_teams.current"] = std::to_string(iaWaves_);
    UiRect fr = ui_.window("ia_fire_teams");
    float wv = float(iaWaves_ - 1) / 9.f;
    if (slider({fr.x + 15, fr.y + 20, 97, 12}, wv)) iaWaves_ = 1 + int(std::lround(wv * 9.f));
    ui_.window("back");
    ui_.window("low_ia_gear");
    ui_.window("low_launch_ia");
    if (ui_.clicked("back.back") || (ui_.keyPressed(SDLK_ESCAPE) && !popup_.open)) go(Scr::Single);
    else if (ui_.clicked("low_ia_gear.btn")) back_ = Scr::Instant, go(Scr::Gear);
    else if (ui_.clicked("low_launch_ia.btn")) launch();
}

// ---------------------------------------------------------------- saves
void Shell::autosave(char kind) {
    CampaignPilot* p = pilot();
    if (!p) return;
    Save s;
    s.mission = msn_.id, s.kind = kind, s.date = nowString(), s.lost = p->lost;
    // one save per mission and kind: the newest replaces the old
    p->saves.erase(std::remove_if(p->saves.begin(), p->saves.end(), [&](const Save& o) { return o.mission == s.mission && o.kind == kind; }),
                   p->saves.end());
    p->saves.push_back(s);
    saveRoster();
}

void Shell::loadRoster() {
    roster_.clear();
    std::ifstream f(app_.prefDir + "pilots.txt");
    std::string line;
    CampaignPilot* p = nullptr;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line == "[pilot]") {
            roster_.push_back({});
            p = &roster_.back();
            continue;
        }
        size_t eq = line.find('=');
        if (!p || eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        if (k == "name") p->name = v;
        else if (k == "chassis") p->chassis = v;
        else if (k == "variant") p->variant = v;
        else if (k == "refit") p->refit = v;
        else if (k == "restat") p->restat = v;
        else if (k == "mission") p->mission = v;
        else if (k == "lost") p->lost = split(v, ',');
        else if (k == "squad") p->squad = split(v, ',');
        else if (k == "kills") p->kills = atoi(v.c_str());
        else if (k == "sorties") p->sorties = atoi(v.c_str());
        else if (k == "deaths") p->deaths = atoi(v.c_str());
        else if (k == "shots") p->shots = atoi(v.c_str());
        else if (k == "hits") p->hits = atoi(v.c_str());
        else if (k == "difficulty") p->difficulty = float(atof(v.c_str()));
        else if (k == "save") {  // mission|kind|date|lost,lost
            auto parts = split(v, '|');
            if (parts.size() >= 3) {
                Save s;
                s.mission = parts[0], s.kind = parts[1].empty() ? 'L' : parts[1][0], s.date = parts[2];
                if (parts.size() > 3) s.lost = split(parts[3], ',');
                p->saves.push_back(s);
            }
        }
    }
    roster_.erase(std::remove_if(roster_.begin(), roster_.end(), [](const CampaignPilot& c) { return c.name.empty(); }), roster_.end());
    cur_ = roster_.empty() ? -1 : 0;
}

void Shell::saveRoster() {
    std::ofstream f(app_.prefDir + "pilots.txt");
    if (!f) return;
    for (auto& p : roster_) {
        f << "[pilot]\nname=" << p.name << "\nchassis=" << p.chassis << "\nvariant=" << p.variant << "\nmission=" << p.mission
          << "\nlost=" << join(p.lost, ',') << "\nsquad=" << join(p.squad, ',') << "\nkills=" << p.kills << "\nsorties=" << p.sorties
          << "\ndeaths=" << p.deaths << "\nshots=" << p.shots << "\nhits=" << p.hits << "\ndifficulty=" << p.difficulty << "\n";
        if (!p.refit.empty()) f << "refit=" << p.refit << "\n";
        if (!p.restat.empty()) f << "restat=" << p.restat << "\n";
        for (auto& s : p.saves) f << "save=" << s.mission << "|" << s.kind << "|" << s.date << "|" << join(s.lost, ',') << "\n";
    }
}

}  // namespace hg2
