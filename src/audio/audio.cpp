#include "audio.h"

#define DR_MP3_IMPLEMENTATION
#include "../../third_party/dr_mp3.h"

#include <algorithm>
#include <cmath>

namespace hg2 {

namespace {
const SDL_AudioSpec kSpec = {SDL_AUDIO_S16, 2, 44100};
}

Audio::~Audio() { shutdown(); }

void Audio::shutdown() {
    for (int i = 0; i < 500 && loadingTrack_; i++) SDL_Delay(10);  // let a track decode finish
    if (stream_) SDL_DestroyAudioStream(stream_);
    stream_ = nullptr;
}

bool Audio::init(const FileSystem& fs) {
    fs_ = &fs;
    if (!SDL_InitSubSystem(SDL_INIT_AUDIO)) {
        SDL_Log("audio: %s", SDL_GetError());
        return false;
    }
    stream_ = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &kSpec, callback, this);
    if (!stream_) {
        SDL_Log("audio: %s", SDL_GetError());
        return false;
    }
    SDL_ResumeAudioStreamDevice(stream_);
    loadSoundList();
    return true;
}

// Sounds.lst: "Sound_Object { file: x.wav  maximum_Volume: 0..10000  outer_Range: decimetres  EAR_PERF_LOOP ... }"
void Audio::loadSoundList() {
    Bytes b;
    if (!fs_->read("Sounds.lst", b)) return;
    std::string txt(b.begin(), b.end()), file;
    SoundInfo cur;
    size_t a = 0;
    while (a < txt.size()) {
        size_t e = txt.find('\n', a);
        if (e == std::string::npos) e = txt.size();
        std::string line = txt.substr(a, e - a);
        a = e + 1;
        size_t h = line.find('#');
        if (h != std::string::npos) line.resize(h);
        auto val = [&](const char* key, float& out) {
            size_t k = line.find(key);
            if (k != std::string::npos) out = float(atof(line.c_str() + k + strlen(key)));
            return k != std::string::npos;
        };
        float v;
        if (line.find("Sound_Object") != std::string::npos) {
            file.clear(), cur = SoundInfo{};
        } else if (line.find("file:") != std::string::npos) {
            size_t k = line.find("file:") + 5;
            while (k < line.size() && isspace((unsigned char)line[k])) k++;
            size_t q = k;
            while (q < line.size() && !isspace((unsigned char)line[q])) q++;
            file = upper(line.substr(k, q - k));
        } else if (val("maximum_Volume:", v)) {
            cur.volume = std::max(0.f, std::min(1.f, v / 10000.f));
        } else if (val("outer_Range:", v)) {
            cur.range = v / 10.f;
        } else if (line.find("EAR_PERF_LOOP") != std::string::npos) {
            cur.loop = true;
        } else if (line.find('}') != std::string::npos && !file.empty()) {
            infos_[file] = cur;
            file.clear();
        }
    }
    SDL_Log("audio: %zu sound objects from Sounds.lst", infos_.size());
}

const Audio::SoundInfo* Audio::info(const std::string& wav) const {
    auto it = infos_.find(upper(wav));
    return it == infos_.end() ? nullptr : &it->second;
}

std::shared_ptr<Audio::Clip> Audio::decodeTrack(const std::string& path) {
    std::shared_ptr<Clip> c;
    std::string ext = lower(extension(path));
    if (ext == "mp3") {
        drmp3_config cfg;
        drmp3_uint64 frames = 0;
        drmp3_int16* pcm = drmp3_open_file_and_read_pcm_frames_s16(path.c_str(), &cfg, &frames, nullptr);
        if (pcm && frames) {
            SDL_AudioSpec src = {SDL_AUDIO_S16, int(cfg.channels), int(cfg.sampleRate)};
            Uint8* out = nullptr;
            int outLen = 0;
            if (SDL_ConvertAudioSamples(&src, reinterpret_cast<Uint8*>(pcm), int(frames * cfg.channels * 2), &kSpec, &out, &outLen)) {
                c = std::make_shared<Clip>();
                c->pcm.assign(reinterpret_cast<int16_t*>(out), reinterpret_cast<int16_t*>(out) + outLen / 2);
                SDL_free(out);
            }
        }
        if (pcm) drmp3_free(pcm, nullptr);
    } else if (ext == "wav") {
        SDL_AudioSpec spec;
        Uint8* buf = nullptr;
        Uint32 len = 0;
        if (SDL_LoadWAV(path.c_str(), &spec, &buf, &len)) {
            Uint8* out = nullptr;
            int outLen = 0;
            if (SDL_ConvertAudioSamples(&spec, buf, int(len), &kSpec, &out, &outLen)) {
                c = std::make_shared<Clip>();
                c->pcm.assign(reinterpret_cast<int16_t*>(out), reinterpret_cast<int16_t*>(out) + outLen / 2);
                SDL_free(out);
            }
            SDL_free(buf);
        }
    }
    return c;
}

bool Audio::startMusic(const std::string& folder) {
    if (!stream_) return false;
    tracks_.clear();
    int n = 0;
    char** list = SDL_GlobDirectory(folder.c_str(), nullptr, SDL_GLOB_CASEINSENSITIVE, &n);
    for (int i = 0; list && i < n; i++) {
        std::string f = list[i], ext = lower(extension(f));
        if (ext == "mp3" || ext == "wav") tracks_.push_back(folder + "/" + f);
    }
    SDL_free(list);
    if (tracks_.empty()) return false;
    for (size_t i = tracks_.size(); i > 1; i--) std::swap(tracks_[i - 1], tracks_[size_t(rand()) % i]);
    musicOn_ = true, trackIdx_ = 0;
    SDL_Log("music: %zu tracks in %s", tracks_.size(), folder.c_str());
    return true;
}

void Audio::stopMusic() {
    musicOn_ = false;
    std::lock_guard<std::mutex> g(mutex_);
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [](const Voice& v) { return v.music; }), voices_.end());
}

std::shared_ptr<Audio::Clip> Audio::clip(const std::string& name) {
    std::string key = upper(name);
    auto it = cache_.find(key);
    if (it != cache_.end()) return it->second;
    std::shared_ptr<Clip> c;
    Bytes b;
    if (fs_->read(key, b)) {
        SDL_AudioSpec spec;
        Uint8* buf = nullptr;
        Uint32 len = 0;
        if (SDL_LoadWAV_IO(SDL_IOFromConstMem(b.data(), b.size()), true, &spec, &buf, &len)) {
            Uint8* out = nullptr;
            int outLen = 0;
            if (SDL_ConvertAudioSamples(&spec, buf, int(len), &kSpec, &out, &outLen)) {
                c = std::make_shared<Clip>();
                c->pcm.assign(reinterpret_cast<int16_t*>(out), reinterpret_cast<int16_t*>(out) + outLen / 2);
                SDL_free(out);
            }
            SDL_free(buf);
        }
    }
    cache_[key] = c;
    return c;
}

void Audio::gains(float volume, const Vec3* at, float range, float& l, float& r) const {
    l = r = volume;
    if (!at) return;
    Vec3 d = *at - listener_;
    float dist = length(d);
    float att = std::max(0.f, 1.f - dist / range);
    att *= att;
    // listener faces -Z rotated by yaw; screen right (left-handed view) = (-cos, 0, sin)
    float pan = dist > 0.1f ? dot(d * (1.f / dist), Vec3(-std::cos(yaw_), 0, std::sin(yaw_))) : 0.f;
    l = volume * att * std::min(1.f, 1.f - pan * 0.6f);
    r = volume * att * std::min(1.f, 1.f + pan * 0.6f);
}

bool Audio::play(const std::string& wav, float volume, const Vec3* at, float range) {
    if (!stream_ || wav.empty()) return false;
    auto c = clip(wav);
    if (!c) return false;
    if (const SoundInfo* si = info(wav)) {
        volume *= si->volume;
        if (si->range > 0) range = std::max(range, si->range);
    }
    float l, r;
    gains(volume * effectsVolume, at, range, l, r);
    if (l <= 0.01f && r <= 0.01f) return true;
    std::lock_guard<std::mutex> g(mutex_);
    if (voices_.size() >= 48) {
        auto it = std::find_if(voices_.begin(), voices_.end(), [](const Voice& v) { return v.loop < 0 && !v.dialog && !v.music; });
        if (it == voices_.end()) return true;
        voices_.erase(it);
    }
    voices_.push_back({c, 0, l, r, false});
    return true;
}

int Audio::startLoop(const std::string& wav, float volume) {
    if (!stream_ || wav.empty()) return -1;
    auto c = clip(wav);
    if (!c || c->pcm.size() < 4) return -1;
    std::lock_guard<std::mutex> g(mutex_);
    int id = nextLoop_++;
    voices_.push_back({c, 0, volume * effectsVolume, volume * effectsVolume, false, id});
    return id;
}

void Audio::setLoop(int handle, float volume, const Vec3* at, float range) {
    if (handle < 0) return;
    float l, r;
    gains(volume * effectsVolume, at, range, l, r);
    std::lock_guard<std::mutex> g(mutex_);
    for (auto& v : voices_)
        if (v.loop == handle) v.l = l, v.r = r;
}

void Audio::stopLoop(int handle) {
    std::lock_guard<std::mutex> g(mutex_);
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [&](const Voice& v) { return v.loop == handle; }), voices_.end());
}

bool Audio::playPcm(const SDL_AudioSpec& spec, const uint8_t* data, size_t bytes, float volume) {
    if (!stream_ || !data || !bytes) return false;
    Uint8* out = nullptr;
    int outLen = 0;
    if (!SDL_ConvertAudioSamples(&spec, data, int(bytes), &kSpec, &out, &outLen)) return false;
    auto c = std::make_shared<Clip>();
    c->pcm.assign(reinterpret_cast<int16_t*>(out), reinterpret_cast<int16_t*>(out) + outLen / 2);
    SDL_free(out);
    flushDialog();
    std::lock_guard<std::mutex> g(mutex_);
    voices_.push_back({c, 0, volume, volume, true});
    return true;
}

bool Audio::dialogPlaying() {
    std::lock_guard<std::mutex> g(mutex_);
    return !dialog_.empty() || std::any_of(voices_.begin(), voices_.end(), [](const Voice& v) { return v.dialog; });
}

void Audio::stopAll() {
    std::lock_guard<std::mutex> g(mutex_);
    dialog_.clear();
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [](const Voice& v) { return !v.music; }), voices_.end());
}

void Audio::flushDialog() {
    std::lock_guard<std::mutex> g(mutex_);
    dialog_.clear();
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [](const Voice& v) { return v.dialog; }), voices_.end());
}

void Audio::queueDialog(const std::string& wav) {
    std::lock_guard<std::mutex> g(mutex_);
    dialog_.push_back(wav);
}

void Audio::update() {
    if (!stream_) return;
    if (musicOn_ && !tracks_.empty()) {
        bool playing;
        {
            std::lock_guard<std::mutex> g(mutex_);
            playing = std::any_of(voices_.begin(), voices_.end(), [](const Voice& v) { return v.music; });
            for (auto& v : voices_)
                if (v.music) v.l = v.r = musicVolume;
        }
        if (!playing && !loadingTrack_) {
            std::lock_guard<std::mutex> g(mutex_);
            if (nextTrack_) {
                voices_.push_back({nextTrack_, 0, musicVolume, musicVolume, false, -1, true});
                nextTrack_.reset();
            } else {
                loadingTrack_ = true;
                std::string path = tracks_[trackIdx_++ % tracks_.size()];
                std::thread([this, path]() {
                    auto c = decodeTrack(path);
                    std::lock_guard<std::mutex> lk(mutex_);
                    nextTrack_ = c ? c : std::make_shared<Clip>();
                    loadingTrack_ = false;
                }).detach();
            }
        }
    }
    std::string next;
    {
        std::lock_guard<std::mutex> g(mutex_);
        bool speaking = std::any_of(voices_.begin(), voices_.end(), [](const Voice& v) { return v.dialog; });
        if (speaking || dialog_.empty()) return;
        next = dialog_.front();
        dialog_.pop_front();
    }
    auto c = clip(next);
    if (!c) return;
    std::lock_guard<std::mutex> g(mutex_);
    voices_.push_back({c, 0, 1.f, 1.f, true});
}

void Audio::mix(int16_t* out, int frames) {
    std::vector<int32_t> acc(size_t(frames) * 2, 0);
    {
        std::lock_guard<std::mutex> g(mutex_);
        for (auto& v : voices_) {
            const auto& pcm = v.clip->pcm;
            for (int f = 0; f < frames && v.pos + 1 < pcm.size(); f++, v.pos += 2) {
                if (v.loop >= 0 && v.pos + 2 >= pcm.size()) v.pos = 0;
                float k = masterVolume * (v.dialog ? dialogVolume : 1.f);
                acc[size_t(f) * 2] += int32_t(float(pcm[v.pos]) * v.l * k);
                acc[size_t(f) * 2 + 1] += int32_t(float(pcm[v.pos + 1]) * v.r * k);
            }
        }
        voices_.erase(std::remove_if(voices_.begin(), voices_.end(),
                                     [](const Voice& v) { return v.loop < 0 && v.pos + 1 >= v.clip->pcm.size(); }),
                      voices_.end());
    }
    for (size_t i = 0; i < acc.size(); i++) out[i] = int16_t(std::max(-32768, std::min(32767, acc[i])));
}

void SDLCALL Audio::callback(void* user, SDL_AudioStream* s, int additional, int) {
    auto* a = static_cast<Audio*>(user);
    int frames = additional / 4;
    if (frames <= 0) return;
    std::vector<int16_t> buf(size_t(frames) * 2);
    a->mix(buf.data(), frames);
    SDL_PutAudioStreamData(s, buf.data(), int(buf.size() * 2));
}

}  // namespace hg2
