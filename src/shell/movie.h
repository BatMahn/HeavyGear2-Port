// AVI movies (the intro / cut-scene movies in Setup\movies and the briefing movies in Shell\movies):
// Intel Indeo 5 video (decoded by third_party/indeo5, from FFmpeg) and PCM sound.
#pragma once
#include <SDL3/SDL.h>

#include <string>
#include <vector>

#include "../assets/assets.h"
#include "../core/fs.h"

struct Indeo5Decoder;

namespace hg2 {

class Movie {
public:
    ~Movie();
    bool open(const std::string& path);       // a file on disk
    bool load(Bytes data);                    // or the bytes of one
    int width() const { return w_; }
    int height() const { return h_; }
    double fps() const { return fps_; }
    int frames() const { return int(video_.size()); }
    // Decodes frames in order up to `index` (frames can only go forward) into RGBA. False at the end / on error.
    bool frame(int index, Image& out);
    const SDL_AudioSpec& audioSpec() const { return spec_; }
    const std::vector<uint8_t>& audio() const { return pcm_; }
    bool hasAudio() const { return !pcm_.empty(); }

private:
    Bytes data_;
    struct Chunk { size_t offset, size; };
    std::vector<Chunk> video_;
    std::vector<uint8_t> pcm_;
    SDL_AudioSpec spec_{};
    int w_ = 0, h_ = 0;
    double fps_ = 15;
    uint32_t fourcc_ = 0;
    Indeo5Decoder* dec_ = nullptr;
    int decoded_ = -1;
    std::vector<uint8_t> y_, u_, v_;
    bool haveFrame_ = false;
    void toRgba(Image& out) const;
};

}  // namespace hg2
