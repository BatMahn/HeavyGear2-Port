// Heavy Gear II port — entry point: finds the game data, opens the window and GPU renderer, then runs the
// shell (menus, campaign, historical, training, instant action). --mission starts one mission directly.
#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <cstdio>
#include <cstdlib>
#include <string>

#include "app.h"
#include "game/session.h"
#include "net/net.h"
#include "shell/gamedata.h"
#include "shell/shell.h"

using namespace hg2;

static void usage() {
    printf("usage: hg2 [--data <dir>] [--mission S01] [--music <dir>] [--window WxH] [--fullscreen] [--no-movies]\n"
           "       [--host <players> | --join <address>] [--name <name>] [--gear <chassis>]   network match\n"
           "  <dir> is your Heavy Gear II folder (the one with simpack.prj and hg2sim.dll), or the CD's\n"
           "  Setup\\data folder. The HG2_DATA environment variable works too; without either, the folder\n"
           "  next to the program (or a 'data' folder there) is used.\n"
           "  --mission starts a mission directly, skipping the menus.\n"
           "test options: --screenshot out.png [--shell <screen>] [--mouse x,y] [--cam x,y,z,yaw,pitch] [--nofog]\n"
           "  [--drive frames] [--fire frames] [--nav-drive] [--external] [--side] [--input f:action,...]\n"
           "  [--start x,z,yaw] [--size WxH] [--verbose-scripts] [--squad n] [--waves n]\n");
}

static bool looksLikeData(const std::string& d) {
    std::string p = d + "/simpack.prj";
    SDL_IOStream* io = SDL_IOFromFile(p.c_str(), "rb");
    if (!io) {
        p = d + "/SIMPACK.PRJ";
        io = SDL_IOFromFile(p.c_str(), "rb");
    }
    if (io) SDL_CloseIO(io);
    return io != nullptr;
}

int main(int argc, char** argv) {
    AppContext app;
    std::string missionName, shot, shellScreen;
    TestOptions test;
    float mouseX = 320, mouseY = 240;
    int squadTest = 0, wavesTest = 0;
    bool forceFull = false, noMovies = false;
    int mpHost = 0;
    std::string mpJoin, mpName = "Player", mpChassis = "Jaguar";
    if (const char* e = SDL_getenv("HG2_DATA")) app.data = e;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        auto next = [&]() { return i + 1 < argc ? std::string(argv[++i]) : std::string(); };
        if (a == "--data") app.data = next();
        else if (a == "--mission") missionName = next();
        else if (a == "--screenshot") shot = next();
        else if (a == "--shell") shellScreen = next();
        else if (a == "--mouse") sscanf(next().c_str(), "%f,%f", &mouseX, &mouseY);
        else if (a == "--nofog") test.noFog = true;
        else if (a == "--verbose-scripts") test.verboseScripts = true;
        else if (a == "--drive") test.driveFrames = atoi(next().c_str());
        else if (a == "--fire") test.fireFrames = atoi(next().c_str());
        else if (a == "--nav-drive") test.navDrive = true;
        else if (a == "--external") test.external = true;
        else if (a == "--side") test.sideView = true;
        else if (a == "--music") app.musicDir = next();
        else if (a == "--squad") squadTest = atoi(next().c_str());
        else if (a == "--waves") wavesTest = atoi(next().c_str());
        else if (a == "--fullscreen") forceFull = true;
        else if (a == "--no-movies") noMovies = true;
        else if (a == "--host") mpHost = std::max(1, atoi(next().c_str()));
        else if (a == "--join") mpJoin = next();
        else if (a == "--name") mpName = next();
        else if (a == "--gear") mpChassis = next();
        else if (a == "--input") {
            std::string list = next();
            for (size_t q = 0; q < list.size();) {
                size_t e = list.find(',', q);
                if (e == std::string::npos) e = list.size();
                std::string item = list.substr(q, e - q);
                size_t c = item.find(':');
                if (c != std::string::npos) test.inputScript.push_back({atoi(item.c_str()), item.substr(c + 1)});
                q = e + 1;
            }
        } else if (a == "--start") sscanf(next().c_str(), "%f,%f,%f", &test.startX, &test.startZ, &test.startYaw);
        else if (a == "--size" || a == "--window") sscanf(next().c_str(), "%dx%d", &app.width, &app.height);
        else if (a == "--cam") {
            if (sscanf(next().c_str(), "%f,%f,%f,%f,%f", &test.cam[0], &test.cam[1], &test.cam[2], &test.cam[3], &test.cam[4]) == 5)
                test.camSet = true;
        } else {
            usage();
            return a == "--help" || a == "-h" ? 0 : 1;
        }
    }
    if (app.data.empty()) {  // next to the program, or a data folder there
        const char* base = SDL_GetBasePath();
        std::string b = base ? base : "./";
        for (const std::string& c : {b + "data", b, b + "../Resources/data", std::string("data"), std::string(".")})
            if (looksLikeData(c)) {
                app.data = c;
                break;
            }
    }
    if (app.data.empty()) {
        usage();
        SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Heavy Gear II",
                                 "Heavy Gear II data not found.\nStart the game with --data <your Heavy Gear II folder> "
                                 "or set HG2_DATA.",
                                 nullptr);
        return 1;
    }
    if (!app.fs.mount(app.data)) {
        fprintf(stderr, "Could not find Heavy Gear II data (simpack.prj/simpack.map) in %s\n", app.data.c_str());
        return 1;
    }
    if (const char* pref = SDL_GetPrefPath("BatMahn", "HG2Port")) {
        app.prefDir = pref;
        SDL_free(const_cast<char*>(pref));
    }
    app.settings.load(app.prefDir + "settings.cfg");
    if (forceFull) app.settings.fullscreen = true;
    app.offscreen = !shot.empty();
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) {
        fprintf(stderr, "SDL_Init: %s\n", SDL_GetError());
        return 1;
    }
    if (!app.offscreen) {
        SDL_WindowFlags fl = SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY | (app.settings.fullscreen ? SDL_WINDOW_FULLSCREEN : 0);
        app.win = SDL_CreateWindow("Heavy Gear II", app.width, app.height, fl);
        if (!app.win) {
            fprintf(stderr, "SDL_CreateWindow: %s\n", SDL_GetError());
            return 1;
        }
    }
    std::string err;
    if (!app.r.init(app.win, app.offscreen, app.width, app.height, &err)) {
        fprintf(stderr, "renderer: %s\n", err.c_str());
        return 1;
    }
    SDL_Log("GPU driver: %s", app.r.driverName());
    if (!app.offscreen && app.audio.init(app.fs)) {
        app.audioOk = true;
        app.applyVolumes();
        if (app.musicDir.empty()) app.musicDir = app.data + "/music";
        if (!app.audio.startMusic(app.musicDir)) app.audio.startMusic(app.data + "/../music");
    }
    int rc = 0;
    if (!shellScreen.empty() || (missionName.empty() && app.offscreen)) {
        Shell shell(app);
        if (!shell.init() || !shell.screenshot(shellScreen.empty() ? "main" : shellScreen, shot, mouseX, mouseY)) rc = 1;
        else printf("wrote %s\n", shot.c_str());
    } else if (mpHost > 0 || !mpJoin.empty()) {
        // network match from the command line: --host <players> or --join <address>, the arena from --mission
        GameData gd;
        gd.load(app.fs);
        const Chassis* ch = gd.findChassis(mpChassis);
        NetGear gear;
        gear.chassis = ch ? ch->index : 0;
        if (ch)  // its first variant's weapons and stats
            for (auto& v : ch->variants) {
                VarRecord rec;
                if (v.hidden || !gd.variantRecord(v.file, rec)) continue;
                for (auto& w : rec.weapons)
                    if (int idx = gd.weaponIndex(w.wpn); idx >= 0) gear.weapons.push_back({w.slot, idx});
                gear.armor = rec.armor, gear.fireControl = rec.fireControl;
                break;
            }
        NetGame net;
        bool ok = mpHost > 0 ? net.host(mpName, gear) : net.join(mpJoin, mpName, gear);
        if (!missionName.empty()) net.arena = missionName;
        printf("%s\n", net.status.c_str());
        Uint64 t0 = SDL_GetTicks();
        while (ok && !net.startReceived && SDL_GetTicks() - t0 < 120000) {
            SDL_Event ev;
            while (SDL_PollEvent(&ev))
                if (ev.type == SDL_EVENT_QUIT) ok = false;
            if (!net.poll()) ok = false;
            if (net.isHost() && int(net.players().size()) >= mpHost) net.start();
            SDL_Delay(10);
        }
        if (!ok || !net.startReceived) {
            fprintf(stderr, "network: %s\n", net.status.c_str());
            rc = 1;
        } else {
            MissionSetup setup;
            setup.mission = net.arena;
            setup.player.chassis = gear.chassis, setup.player.weapons = gear.weapons;
            setup.player.armor = gear.armor, setup.player.fireControl = gear.fireControl;
            setup.net = &net;
            MissionResult r = runMission(app, setup, &test);
            printf("match over: %s\n", r.outcome == MissionResult::Success ? "won" : "lost");
            for (auto& p : net.players()) printf("  %s %d/%d\n", p.name.c_str(), p.kills, p.deaths);
            net.stop();
        }
    } else if (!missionName.empty()) {
        MissionSetup setup;
        setup.mission = missionName;
        // test: squad mates / instant-action waves on any mission
        for (int i = 0; i < squadTest; i++) setup.squad.push_back({"Mate " + std::to_string(i + 1), {}});
        if (wavesTest > 0) {
            setup.waves = wavesTest;
            setup.enemies.push_back({"Enemy 1", {36, {}}});
            setup.enemies.push_back({"Enemy 2", {13, {}}});
        }
        if (app.offscreen) test.screenshot = shot;
        MissionResult r = runMission(app, setup, &test);
        if (r.outcome == MissionResult::Error) {
            fprintf(stderr, "%s\n", r.error.c_str());
            rc = 1;
        }
        if (!app.offscreen) app.settings.save(app.prefDir + "settings.cfg");
    } else {
        Shell shell(app);
        if (!shell.init()) {
            SDL_ShowSimpleMessageBox(SDL_MESSAGEBOX_ERROR, "Heavy Gear II",
                                     "The shell data was not found (hg2shell.prj and the shell artwork: Shell\\shellart\\*.zfs "
                                     "in an installed game, or Setup\\zip\\zfs.zip on the CD).\n"
                                     "Missions can still be started with --mission <name>.",
                                     app.win);
            rc = 1;
        } else {
            shell.setMovies(!noMovies);
            shell.run();
        }
    }
    app.audio.shutdown();
    app.r.shutdown();
    if (app.win) SDL_DestroyWindow(app.win);
    SDL_Quit();
    return rc;
}
