// Sound: WAV effects and dialog from the game archives, mixed through an SDL3 audio stream.
#pragma once
#include <SDL3/SDL.h>

#include <atomic>
#include <deque>
#include <memory>
#include <mutex>
#include <thread>
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
    // Positional when `at` is given, otherwise centred. Returns false if the sound can't be found.
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
    // Per-sound parameters from Sounds.lst (EAR sound objects): maximum volume, audible range, looping.
    struct SoundInfo { float volume = 1.f, range = 0.f; bool loop = false; };
    const SoundInfo* info(const std::string& wav) const;
    // Music: ripped CD tracks (.mp3 / .wav) in a folder, played in shuffled order. False if none were found.
    bool startMusic(const std::string& folder);
    void stopMusic();

private:
    struct Clip { std::vector<int16_t> pcm; };  // stereo 44.1 kHz
    struct Voice { std::shared_ptr<Clip> clip; size_t pos; float l, r; bool dialog; int loop = -1; bool music = false; };
    std::unordered_map<std::string, SoundInfo> infos_;
    void loadSoundList();
    std::vector<std::string> tracks_;
    size_t trackIdx_ = 0;
    std::shared_ptr<Clip> nextTrack_;
    std::atomic<bool> loadingTrack_{false};
    bool musicOn_ = false;
    std::shared_ptr<Clip> decodeTrack(const std::string& path);
    void gains(float volume, const Vec3* at, float range, float& l, float& r) const;
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
