#include "audio.h"

#define DR_MP3_IMPLEMENTATION
#include "../../third_party/dr_mp3.h"

#include <algorithm>
#include <cmath>

namespace hg2 {

namespace {
const SDL_AudioSpec kSpec = {SDL_AUDIO_S16, 2, 44100};

// EAR's pan matrices (earpds RegisterMatrix): 128 entries of {DirectSound pan in mB, level scale}. The game registers
// matrix 0x40 (built by FUN_10014258) and matrix 1 (FUN_10012496). Matrix 0x40 is a set of rings: entry 0 the
// centre, then 4, 8, 16, 32 and 64 directions starting at entries 1, 5, 13, 29 and 61 (2^k - 3); in front a source
// is played at 0.83, behind at 0.45, to the side up to 1.0 and fully in one speaker. The values were produced by
// running the DLL's own builders (constants 1.0 / 0.83 / 0.6 / 0.45 and the gain-to-mB polynomial).
struct MatrixEntry { int pan; float gain; };
const MatrixEntry kMatrix40[128] = {
{0, 1.0000f}, {-203, 0.8300f}, {-203, 0.8300f}, {203, 0.6000f}, {203, 0.6000f}, {-203, 0.4688f}, 
{-203, 0.6600f}, {-203, 0.8512f}, {0, 0.8300f}, {203, 0.8512f}, {203, 0.6600f}, {203, 0.4688f}, 
{0, 0.4500f}, {-203, 0.4688f}, {-500, 0.4875f}, {-500, 0.6017f}, {-500, 0.7158f}, {-500, 0.8583f}, 
{-500, 0.8725f}, {-203, 0.8512f}, {0, 0.8300f}, {203, 0.8512f}, {500, 0.8725f}, {500, 0.8583f}, 
{500, 0.7158f}, {500, 0.6017f}, {500, 0.4875f}, {203, 0.4688f}, {0, 0.4500f}, {-203, 0.4688f}, 
{-500, 0.4875f}, {-765, 0.5063f}, {-999, 0.5250f}, {-999, 0.5860f}, {-999, 0.6470f}, {-999, 0.7080f}, 
{-999, 0.7690f}, {-999, 0.8640f}, {-999, 0.8810f}, {-999, 0.8980f}, {-999, 0.9150f}, {-765, 0.8938f}, 
{-500, 0.8725f}, {-203, 0.8512f}, {0, 0.8300f}, {203, 0.8512f}, {500, 0.8725f}, {765, 0.8938f}, 
{999, 0.9150f}, {999, 0.8980f}, {999, 0.8810f}, {999, 0.8640f}, {999, 0.7690f}, {999, 0.7080f}, 
{999, 0.6470f}, {999, 0.5860f}, {999, 0.5250f}, {765, 0.5063f}, {500, 0.4875f}, {203, 0.4688f}, 
{0, 0.4500f}, {-203, 0.4688f}, {-500, 0.4875f}, {-765, 0.5063f}, {-999, 0.5250f}, {-1328, 0.5437f}, 
{-2000, 0.5625f}, {-3390, 0.5813f}, {-10000, 0.6000f}, {-10000, 0.6213f}, {-10000, 0.6425f}, {-10000, 0.6638f}, 
{-10000, 0.6850f}, {-10000, 0.7063f}, {-10000, 0.7275f}, {-10000, 0.7488f}, {-10000, 0.7700f}, {-10000, 0.8512f}, 
{-10000, 0.8725f}, {-10000, 0.8938f}, {-10000, 0.9150f}, {-10000, 0.9362f}, {-10000, 0.9575f}, {-10000, 0.9787f}, 
{-10000, 1.0000f}, {-3390, 0.9787f}, {-2000, 0.9575f}, {-1328, 0.9362f}, {-999, 0.9150f}, {-765, 0.8938f}, 
{-500, 0.8725f}, {-203, 0.8512f}, {0, 0.8300f}, {203, 0.8512f}, {500, 0.8725f}, {765, 0.8938f}, 
{999, 0.9150f}, {1328, 0.9362f}, {2000, 0.9575f}, {3390, 0.9787f}, {10000, 1.0000f}, {10000, 0.9787f}, 
{10000, 0.9575f}, {10000, 0.9362f}, {10000, 0.9150f}, {10000, 0.8938f}, {10000, 0.8725f}, {10000, 0.8512f}, 
{10000, 0.7700f}, {10000, 0.7488f}, {10000, 0.7275f}, {10000, 0.7063f}, {10000, 0.6850f}, {10000, 0.6638f}, 
{10000, 0.6425f}, {10000, 0.6213f}, {10000, 0.6000f}, {3390, 0.5813f}, {2000, 0.5625f}, {1328, 0.5437f}, 
{999, 0.5250f}, {765, 0.5063f}, {500, 0.4875f}, {203, 0.4688f}, {0, 0.4500f}, {0, 0.8300f}, 
{0, 0.8867f}, {0, 0.9433f}
};
const MatrixEntry kMatrix1[21] = {{0, 1.00f}, {-10000, 0.60f}, {-10000, 0.83f}, {-10000, 1.00f}, {0, 0.83f}, {10000, 1.00f}, {10000, 0.83f}, {10000, 0.60f}, {0, 0.45f}, {0, 1.00f}, {0, 0.83f}, {-10000, 1.00f}, {10000, 1.00f}, {-5000, 1.00f}, {5000, 1.00f}, {0, 1.00f}, {0, 0.83f}, {0, 1.00f}, {0, 1.00f}, {0, 1.00f}, {0, 1.00f}};
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

// Sounds.lst: "Sound_Object { file: x.wav  channel: n  minimum_Volume / maximum_Volume: 0..10000  outer_Range /
// inner_Range: centimetres  priority: n  EAR_PERF_DISTANCE  EAR_PERF_LOOP ... }" (FUN_1012d380: a looping sound on a
// channel below 5 goes to channel 2)
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
            if (k != std::string::npos) out = float(atoi(line.c_str() + k + strlen(key)));  // "%i"
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
            cur.maxVol = std::min(10000.f, v);
        } else if (val("minimum_Volume:", v)) {
            cur.minVol = v;
        } else if (val("outer_Range:", v)) {
            cur.outer = v / 100.f;
        } else if (val("inner_Range:", v)) {
            cur.inner = v / 100.f;
        } else if (val("channel:", v)) {
            cur.channel = int(v);
        } else if (val("priority:", v)) {
            cur.priority = int(v);
        } else if (val("matrix:", v)) {
            cur.matrix = int(v);
        } else if (val("azimut:", v)) {
            cur.azimut = int(v);
        } else if (line.find("Ring_") != std::string::npos) {
            size_t k = line.find("Ring_") + 5;
            int idx = line[k] - '0';
            if (idx >= 0 && idx < 7 && val(":", v)) cur.ring[idx] = int(v);
        } else if (line.find("EAR_PERF_PRIORITIZE") != std::string::npos) {
            cur.prioritize = true;
        } else if (line.find("EAR_PERF_UPDATE") != std::string::npos) {
            cur.directional = true;  // flag 0x10
        } else if (line.find("EAR_PERF_DISTANCE") != std::string::npos) {
            cur.distance = true, cur.directional = true;  // flags 0x11
        } else if (line.find("EAR_PERF_LOOP") != std::string::npos) {
            cur.loop = true;
            if (cur.channel < 5) cur.channel = 2;
        } else if (line.find('}') != std::string::npos && !file.empty()) {
            if (cur.loop && cur.channel < 5) cur.channel = 2;
            cur.volume = cur.maxVol / 10000.f, cur.range = cur.outer;
            infos_[file] = cur;
            file.clear();
        }
    }
    SDL_Log("audio: %zu sound objects from Sounds.lst", infos_.size());
}

float Audio::earLevel(const SoundInfo& si, float dist) {
    if (!si.distance || dist <= si.inner) return si.maxVol;
    if (dist >= si.outer) return si.minVol;
    float v = si.maxVol - std::floor((dist - si.inner) * (si.maxVol - si.minVol) / std::max(0.01f, si.outer - si.inner));
    return v >= 10000.f ? 10000.f : v;
}

float Audio::earAmplitude(float level) {
    float x = std::max(0.f, std::min(100.f, level / 100.f));
    float db = -(60.f + x * (-2.73303f + x * (0.0586456f + x * (-0.00058625f + x * 2.131e-06f))));
    return std::min(1.f, std::pow(10.f, db / 20.f));
}

float Audio::loopLevel(const std::string& wav, const Vec3& at) const {
    static const SoundInfo kDefault;
    const SoundInfo* si = info(wav);
    return earLevel(si ? *si : kDefault, length(at - listener_));
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
    tracks_.clear(), numbered_.clear(), pinned_.clear();
    int n = 0;
    char** list = SDL_GlobDirectory(folder.c_str(), nullptr, SDL_GLOB_CASEINSENSITIVE, &n);
    for (int i = 0; list && i < n; i++) {
        std::string f = list[i], ext = lower(extension(f));
        if (ext != "mp3" && ext != "wav") continue;
        tracks_.push_back(folder + "/" + f);
        // a track number: the first run of digits in the name ("track02", "02 - Title", "Track 2")
        std::string stem = f.substr(0, f.size() - ext.size() - 1);
        size_t d = stem.find_first_of("0123456789");
        if (d != std::string::npos) {
            int num = atoi(stem.c_str() + d);
            if (num > 0 && num < 100 && !numbered_.count(num)) numbered_[num] = folder + "/" + f;
        }
    }
    SDL_free(list);
    if (tracks_.empty()) return false;
    for (size_t i = tracks_.size(); i > 1; i--) std::swap(tracks_[i - 1], tracks_[size_t(rand()) % i]);
    musicOn_ = true, trackIdx_ = 0;
    SDL_Log("music: %zu tracks in %s", tracks_.size(), folder.c_str());
    return true;
}

bool Audio::playTrack(int n) {
    auto it = numbered_.find(n);
    if (!musicOn_ || it == numbered_.end()) return false;
    std::lock_guard<std::mutex> g(mutex_);
    if (pinned_ == it->second) return true;  // already playing it
    pinned_ = it->second;
    voices_.erase(std::remove_if(voices_.begin(), voices_.end(), [](const Voice& v) { return v.music; }), voices_.end());
    nextTrack_.reset();
    SDL_Log("music: CD track %d -> %s", n, pinned_.c_str());
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
    // redbkNNN.wav are CD-audio cues (Sounds.lst: "#CD audio track N"); no such files ship, the music comes from the
    // ripped tracks (startMusic), so they are not reported
    if (!c && upper(name).rfind("REDBK", 0) != 0) SDL_Log("sound not found: %s", name.c_str());
    cache_[key] = c;
    return c;
}

void Audio::matrixEntry(const SoundInfo& si, const Vec3* rel, int& pan, float& gain) {
    // FUN_10004234: an event without the 0x10 flag keeps its fixed entry (Sounds.lst "azimut:", default 0); with it,
    // matrix 1 always takes entry 0 and matrix 0x40 picks a ring by the distance (Ring_0 .. Ring_6, cm): the search
    // stops at the first pair with ring[j] <= d < ring[j + 1] (k = j + 1, else k = 6); k < 2 is the centre, otherwise
    // 2^k directions from entry 2^k - 3 (FUN_100049a0).
    int idx = si.azimut;
    if (si.directional && rel) {
        idx = 0;
        if (si.matrix == 0x40) {
            // listener-relative centimetres, x to the right, z ahead (FUN_1012bf2e: offsets x 100, rounded)
            int x = int(std::floor(rel->x * 100.f + 0.5f)), z = int(std::floor(rel->z * 100.f + 0.5f));
            int d = int(std::sqrt(float(x) * float(x) + rel->y * rel->y * 1e4f + float(z) * float(z)));
            int k = 0;
            bool found = false;
            do {
                int j = k++;
                if (si.ring[j] <= d && d < si.ring[k]) found = true;
            } while (k < 6 && !found);
            if (k >= 2) {
                int n = 1 << k, base = n - 3;
                // quadrant: ahead 2 (left) / 3 (right), behind or level 1 (left) / 4 (right); the angle inside it
                // from the z axis (2, 4) or the x axis (1, 3), in n steps of 6.282 / n from 0.78525
                int q = z < 1 ? (x >= 0 ? 4 : 1) : (x > 0 ? 3 : 2);
                if (n == 4) {
                    idx = q;
                } else {
                    double ax = std::fabs(double(x)), az = std::fabs(double(z));
                    double a = (q == 2 || q == 4) ? std::atan2(ax, az) : std::atan2(az, ax);
                    int f = int(std::floor((a - 0.78525) / (6.282 / n)));
                    idx = ((n >> 3) - 1 + (q - 1) * (n >> 2) - f) % n + base;
                }
            }
        }
    }
    if (si.matrix == 1) {
        const MatrixEntry& e = kMatrix1[idx >= 0 && idx < 21 ? idx : 0];
        pan = e.pan, gain = e.gain;
    } else {
        const MatrixEntry& e = kMatrix40[idx & 127];
        pan = e.pan, gain = e.gain;
    }
}

void Audio::gains(float volume, const Vec3* at, const SoundInfo& si, float& l, float& r, float* levelOut) const {
    float level = at ? earLevel(si, length(*at - listener_)) : si.maxVol;
    if (levelOut) *levelOut = level;
    // the matrix entry scales the level and pans the voice: DirectSound pan p < 0 lowers the right channel by -p mB,
    // p > 0 the left by p mB
    int pan = 0;
    float gain = 1.f;
    Vec3 rel;
    if (at) {
        // listener faces -Z rotated by yaw; screen right (left-handed view) = (-cos, 0, sin)
        Vec3 d = *at - listener_;
        rel = Vec3(dot(d, Vec3(-std::cos(yaw_), 0, std::sin(yaw_))), d.y, dot(d, Vec3(-std::sin(yaw_), 0, -std::cos(yaw_))));
    }
    matrixEntry(si, at ? &rel : nullptr, pan, gain);
    float amp = level * volume * gain > 0 ? earAmplitude(level * volume * gain) : 0.f;
    l = amp * (pan > 0 ? std::pow(10.f, -float(pan) / 2000.f) : 1.f);
    r = amp * (pan < 0 ? std::pow(10.f, float(pan) / 2000.f) : 1.f);
}

int Audio::channelVoices(int channel) const {
    // FUN_1012e708's RegisterChannel calls: (channel, flags, volume, events | voices << 16); a zero half is no limit
    // (earpds RegisterChannel stores 0xffff). The event count (+0x34) is checked when an event is mixed (MixEvent,
    // against the channel's events +0x66, skipped for queued channels) and the voice count (+0x36) when one starts
    // playing (FUN_10004ee3, against +0x38); both stop the same victim (lowest priority, else quietest).
    int full = soundDetail == 0 ? 4 : soundDetail == 1 ? 8 : 16;
    switch (channel) {
        case 1: return 1 << 30;                // flags 3 (queued), 10 events, voices unlimited
        case 2: return std::min(80, full);     // flags 0x10, the sound manager's 0x50 events
        case 3: return std::min(24, full);     // flags 0x30, 0x18 events
        case 4: return std::min(16, full);     // flags 0x71, 0x10 events
        case 5: return 1 << 30;                // flags 7 (queued), 1 event, voices unlimited: the CD music cues
        case 6: return std::min(32, full);     // flags 0x70, 0x20 events
        case 7: return std::min(16, full / 2); // flags 0x70, 0x10 events
        case 8: return 4;                      // flags 0x70, 4 events, voices unlimited: the ambient wind / rain / city
        case 9: return std::min(4, full / 2);  // flags 0x70, 4 events
        default: return 1 << 30;
    }
}

bool Audio::makeRoom(int channel, int priority, float level) {
    // earpds FUN_10004ee3, an event waiting to start on a full channel: the event of lowest non-zero priority below the
    // new one's is stopped; failing that, the quietest (EAR level) if it is quieter than the new one and its priority
    // is no higher; failing that the new event is dropped (error 0x26). With neither a priority nor a level it is
    // dropped at once.
    int count = 0;
    for (auto& v : voices_)
        if (v.channel == channel && !v.dialog && !v.music) count++;
    if (count < channelVoices(channel)) return true;
    if (priority == 0 && level <= 0) return false;
    int lowPri = priority, quietPri = 0;
    float quiet = level;
    auto byPri = voices_.end(), byLevel = voices_.end();
    for (auto it = voices_.begin(); it != voices_.end(); ++it) {
        if (it->channel != channel || it->dialog || it->music) continue;
        if (it->priority < lowPri && it->priority != 0) lowPri = it->priority, byPri = it;
        if (it->level < quiet) quiet = it->level, quietPri = it->priority, byLevel = it;
    }
    if (byPri != voices_.end()) {
        voices_.erase(byPri);
        return true;
    }
    if (byLevel != voices_.end() && quietPri <= priority) {
        voices_.erase(byLevel);
        return true;
    }
    return false;
}

bool Audio::play(const std::string& wav, float volume, const Vec3* at, float range) {
    if (lower(wav).rfind("redbk", 0) == 0) return playTrack(atoi(lower(wav).c_str() + 5));  // music cue (CD track)
    if (wav.empty()) return false;
    if (!stream_) return fs_ && clip(wav), false;  // no device: still resolve the name (reports missing files)
    auto c = clip(wav);
    if (!c) return false;
    static const SoundInfo kDefault;
    const SoundInfo* si = info(wav);
    const SoundInfo& s = si ? *si : kDefault;
    float l, r, level = 0;
    gains(volume * effectsVolume, at, s, l, r, &level);
    if (l <= 0.01f && r <= 0.01f) return true;
    std::lock_guard<std::mutex> g(mutex_);
    int pri = s.prioritize ? s.priority : 0;
    if (!makeRoom(s.channel, pri, level)) return true;
    voices_.push_back({c, 0, l, r, false});
    voices_.back().channel = s.channel, voices_.back().priority = pri, voices_.back().level = level;
    return true;
}

int Audio::startLoop(const std::string& wav, float volume) {
    if (!stream_ || wav.empty()) return -1;
    auto c = clip(wav);
    if (!c || c->pcm.size() < 4) return -1;
    static const SoundInfo kDefault;
    const SoundInfo& s = info(wav) ? *info(wav) : kDefault;
    std::lock_guard<std::mutex> g(mutex_);
    int pri = s.prioritize ? s.priority : 0;
    if (!makeRoom(s.channel, pri, s.maxVol)) return -1;
    int id = nextLoop_++;
    float a = volume > 0 ? earAmplitude(s.maxVol * volume * effectsVolume) : 0.f;
    voices_.push_back({c, 0, a, a, false, id});
    voices_.back().name = wav, voices_.back().channel = s.channel, voices_.back().priority = pri, voices_.back().level = s.maxVol;
    return id;
}

void Audio::setLoop(int handle, float volume, const Vec3* at, float range) {
    if (handle < 0) return;
    float l = 0, r = 0, level = 0;
    if (volume > 0) {
        // the loop's own sound object: find it by the voice's clip
        std::string name;
        {
            std::lock_guard<std::mutex> g(mutex_);
            for (auto& v : voices_)
                if (v.loop == handle) name = v.name;
        }
        static const SoundInfo kDefault;
        const SoundInfo* si = info(name);
        gains(volume * effectsVolume, at, si ? *si : kDefault, l, r, &level);
    }
    std::lock_guard<std::mutex> g(mutex_);
    for (auto& v : voices_)
        if (v.loop == handle) {
            // a new target restarts the glide at its first step (earpds FUN_1000ac13 sets +0x6e to 1)
            if (v.tl < 0) v.ol = v.l, v.or_ = v.r;
            if (v.tl != l || v.tr != r) v.tl = l, v.tr = r, v.glide = 1;
            v.level = level;
        }
}

void Audio::glideTick() {
    // earpds FUN_1000ab47 each 50 ms (timeSetEvent(0x32)) with the step count N = 2 (InitializeEar without flag
    // 0x200, FUN_10004b29): at step 0 or N the old values become the targets; the voice gets
    // old + (target - old) x {1, 0.5, 1}[step] (FUN_1001528c: SetVolume and SetPan from the same table), and the step
    // advances while it is <= N. So a change is half done after one tick and complete after two.
    static const float kStep[] = {1.f, 0.5f, 1.f};
    const int N = 2;
    for (auto& v : voices_) {
        if (v.tl < 0 || v.glide > N + 1) continue;
        if (v.glide == 0 || v.glide == N) v.ol = v.tl, v.or_ = v.tr;
        float t = kStep[std::min(v.glide, N)];
        v.l = v.ol + (v.tl - v.ol) * t, v.r = v.or_ + (v.tr - v.or_) * t;
        if (v.glide <= N) v.glide++;
    }
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
    // "redbkNNN.wav" is a music cue (CD audio track NNN), not a file
    std::string l = lower(wav);
    if (getenv("HG2_DEBUG")) SDL_Log("dialog queued: %s", wav.c_str());
    if (l.rfind("redbk", 0) == 0) {
        playTrack(atoi(l.c_str() + 5));
        return;
    }
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
                std::string path = !pinned_.empty() ? pinned_ : tracks_[trackIdx_++ % tracks_.size()];
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
        for (glideFrames_ += frames; glideFrames_ >= kSpec.freq / 20; glideFrames_ -= kSpec.freq / 20) glideTick();
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
