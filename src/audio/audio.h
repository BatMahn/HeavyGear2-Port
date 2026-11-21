// Sound: WAV effects and dialog from the game archives, mixed through an SDL3 audio stream.
#pragma once
#include <SDL3/SDL.h>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
#include <map>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/fs.h"
#include "../core/math.h"

namespace hg2 {

class Audio {
public:
    ~Audio();
    bool init(const FileSystem& fs);
    void setListener(const Vec3& pos, float yaw) { listener_ = pos, yaw_ = yaw; }
    // Positional when `at` is given, otherwise centred. Returns false if the sound can't be found. Levels and ranges
    // are the sound's own (Sounds.lst); `volume` scales the level, `range` is ignored.
    bool play(const std::string& wav, float volume = 1.f, const Vec3* at = nullptr, float range = 300.f);
    void queueDialog(const std::string& wav);  // played one after another
    void flushDialog();                         // drop queued and playing dialog
    // Looping sounds (engines): returns a handle or -1; volume 0 keeps the loop silent.
    int startLoop(const std::string& wav, float volume);
    void setLoop(int handle, float volume, const Vec3* at = nullptr, float range = 300.f);
    void stopLoop(int handle);
    void update();
    bool ok() const { return stream_ != nullptr; }
    float masterVolume = 0.8f;
    float musicVolume = 0.5f;
    float effectsVolume = 1.f, dialogVolume = 1.f;  // scale effects / loops and speech
    // Raw PCM (any SDL format) played once as dialog (briefing audio). Replaces playing dialog.
    bool playPcm(const SDL_AudioSpec& spec, const uint8_t* data, size_t bytes, float volume = 1.f);
    bool dialogPlaying();
    void stopAll();  // everything except music (between screens and missions)
    void shutdown();  // close the device (before SDL_Quit)
    // Per-sound parameters from Sounds.lst (EAR sound objects, parsed by FUN_1012d380): volumes 0..10000, ranges in
    // metres (the file's centimetres: FUN_1012bf2e hands EAR listener-relative positions x 100), channel, priority.
    // Defaults as the parser's: minimum 0, maximum 10000, outer 100 m, inner 0, channel 1.
    struct SoundInfo {
        float minVol = 0, maxVol = 10000, outer = 100.f, inner = 0;
        int channel = 1, priority = 0;
        bool loop = false, distance = false;
        float volume = 1.f, range = 100.f;  // maxVol / 10000 and outer, for callers
    };
    const SoundInfo* info(const std::string& wav) const;
    // Music: ripped CD tracks (.mp3 / .wav) in a folder, played in shuffled order. False if none were found.
    bool startMusic(const std::string& folder);
    // Red Book music: the original played CD audio tracks (mission RedBook_Track, script cues "redbkNNN.wav").
    // Files in the music folder named with a track number (track02.mp3, 02 - Title.mp3, Track 2.wav) stand in for
    // the disc's tracks; playTrack loops that one. Without numbered files the folder plays shuffled.
    bool playTrack(int n);
    void stopMusic();

private:
    struct Clip { std::vector<int16_t> pcm; };  // stereo 44.1 kHz
    struct Voice { std::shared_ptr<Clip> clip; size_t pos; float l, r; bool dialog; int loop = -1; bool music = false; std::string name; };
    std::unordered_map<std::string, SoundInfo> infos_;
    void loadSoundList();
    std::vector<std::string> tracks_;
    size_t trackIdx_ = 0;
    std::map<int, std::string> numbered_;  // CD track number -> file
    std::string pinned_;                   // the cued track, looped
    std::shared_ptr<Clip> nextTrack_;
    std::atomic<bool> loadingTrack_{false};
    bool musicOn_ = false;
    std::shared_ptr<Clip> decodeTrack(const std::string& path);
    void gains(float volume, const Vec3* at, const SoundInfo& si, float& l, float& r) const;
  public:
    // EAR's level for a sound at a distance (FUN_10004504): the maximum inside the inner range, falling in a straight
    // line to the minimum at the outer range (EAR_PERF_DISTANCE sounds only), 0..10000.
    static float earLevel(const SoundInfo& si, float dist);
    // The DirectSound attenuation EAR gives a level (FUN_1001528c): -(60 + a polynomial of the level in percent) dB.
    static float earAmplitude(float level);
    float loopLevel(const std::string& wav, const Vec3& at) const;  // a positional sound's EAR level here
  private:
    int nextLoop_ = 1;
    std::shared_ptr<Clip> clip(const std::string& name);
    static void SDLCALL callback(void* user, SDL_AudioStream* s, int additional, int total);
    void mix(int16_t* out, int frames);

    const FileSystem* fs_ = nullptr;
    SDL_AudioStream* stream_ = nullptr;
    std::mutex mutex_;
    std::vector<Voice> voices_;
    std::unordered_map<std::string, std::shared_ptr<Clip>> cache_;
    std::deque<std::string> dialog_;
    Vec3 listener_;
    float yaw_ = 0;
};

}  // namespace hg2
