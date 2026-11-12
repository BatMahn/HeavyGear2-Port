// What the shell and the missions share: data files, the window and GPU renderer, sound, and the player's
// settings (saved in the user's preference folder).
#pragma once
#include <SDL3/SDL.h>

#include <string>

#include "audio/audio.h"
#include "core/fs.h"
#include "render/renderer.h"

namespace hg2 {

struct Settings {
    float effects = 0.8f, dialog = 1.0f, music = 0.5f, shell = 0.8f;  // 0..1
    bool externalView = false;   // start missions in the chase view
    bool invertMouse = false;
    float mouseSpeed = 1.0f;     // 0.25 .. 3
    int particles = 2;           // 0 off, 1 reduced, 2 full
    bool fullscreen = false;
    bool load(const std::string& path);
    bool save(const std::string& path) const;
};

struct AppContext {
    FileSystem fs;
    Renderer r;
    SDL_Window* win = nullptr;
    Audio audio;
    bool audioOk = false;
    bool offscreen = false;
    int width = 1280, height = 720;
    std::string data, musicDir, prefDir;  // prefDir ends with a separator
    Settings settings;
    bool quit = false;                    // window closed
    void applyVolumes() {
        audio.masterVolume = 1.f;
        audio.effectsVolume = settings.effects;
        audio.dialogVolume = settings.dialog;
        audio.musicVolume = settings.music;
    }
};

}  // namespace hg2
