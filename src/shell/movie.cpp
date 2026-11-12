#include "movie.h"

#include <algorithm>
#include <cstring>
#include <functional>

#include "../../third_party/indeo5/indeo5dec.h"

namespace hg2 {

namespace {
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }
constexpr uint32_t fcc(const char* s) { return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 | uint32_t(uint8_t(s[3])) << 24; }
}  // namespace

Movie::~Movie() {
    if (dec_) indeo5_close(dec_);
}

bool Movie::open(const std::string& path) {
    Bytes b;
    return readFile(path, b) && load(std::move(b));
}

bool Movie::load(Bytes data) {
    data_ = std::move(data);
    const Bytes& d = data_;
    if (d.size() < 12 || rd32(&d[0]) != fcc("RIFF") || rd32(&d[8]) != fcc("AVI ")) return false;
    int stream = -1, videoStream = -1, audioStream = -1;
    uint32_t lastType = 0;
    uint16_t afmt = 0, ach = 0, abits = 0;
    uint32_t arate = 0;
    // depth-first walk of the RIFF tree
    std::function<void(size_t, size_t, int)> walk = [&](size_t p, size_t end, int depth) {
        while (p + 8 <= end && depth < 8) {
            uint32_t id = rd32(&d[p]), sz = rd32(&d[p + 4]);
            size_t body = p + 8;
            if (body + sz > d.size()) sz = uint32_t(d.size() - body);
            size_t next = body + sz + (sz & 1);
            if (id == fcc("LIST") || id == fcc("RIFF")) {
                if (sz >= 4) {
                    if (rd32(&d[body]) == fcc("strl")) stream++;
                    walk(body + 4, body + sz, depth + 1);
                }
            } else if (id == fcc("avih") && sz >= 40) {
                uint32_t usPerFrame = rd32(&d[body]);
                if (usPerFrame) fps_ = 1e6 / double(usPerFrame);
            } else if (id == fcc("strh") && sz >= 24) {
                lastType = rd32(&d[body]);
                if (lastType == fcc("vids") && videoStream < 0) {
                    videoStream = stream, fourcc_ = rd32(&d[body + 4]);
                    uint32_t scale = rd32(&d[body + 20]), rate = rd32(&d[body + 24]);
                    if (scale && rate) fps_ = double(rate) / double(scale);
                }
                if (lastType == fcc("auds") && audioStream < 0) audioStream = stream;
            } else if (id == fcc("strf")) {
                if (stream == videoStream && sz >= 40) {
                    w_ = int(rd32(&d[body + 4])), h_ = std::abs(int(rd32(&d[body + 8])));
                    if (!fourcc_ || fourcc_ == fcc("iv50") || fourcc_ == fcc("IV50")) fourcc_ = rd32(&d[body + 16]);
                } else if (stream == audioStream && sz >= 16 && afmt == 0) {
                    afmt = rd16(&d[body]), ach = rd16(&d[body + 2]), arate = rd32(&d[body + 4]), abits = rd16(&d[body + 14]);
                }
            } else {
                char c[4] = {char(id), char(id >> 8), char(id >> 16), char(id >> 24)};
                if (c[0] >= '0' && c[0] <= '9' && c[1] >= '0' && c[1] <= '9') {
                    int s = (c[0] - '0') * 10 + (c[1] - '0');
                    if (s == videoStream && c[2] == 'd') video_.push_back({body, sz});
                    else if (s == audioStream && c[2] == 'w' && c[3] == 'b') pcm_.insert(pcm_.end(), d.begin() + long(body), d.begin() + long(body + sz));
                }
            }
            p = next;
        }
    };
    walk(12, d.size(), 0);
    if (afmt == 1 && ach && arate) {
        spec_.format = abits == 8 ? SDL_AUDIO_U8 : SDL_AUDIO_S16LE;
        spec_.channels = ach;
        spec_.freq = int(arate);
    } else {
        pcm_.clear();
    }
    if (w_ <= 0 || h_ <= 0 || video_.empty()) return false;
    if (fourcc_ != fcc("IV50") && fourcc_ != fcc("iv50")) {
        SDL_Log("movie: unsupported video codec %.4s", reinterpret_cast<const char*>(&fourcc_));
        return false;
    }
    dec_ = indeo5_open(w_, h_);
    y_.assign(size_t(w_) * h_, 0);
    int cw = (w_ + 3) / 4, ch = (h_ + 3) / 4;
    u_.assign(size_t(cw) * ch, 128), v_.assign(size_t(cw) * ch, 128);
    return dec_ != nullptr;
}

bool Movie::frame(int index, Image& out) {
    if (!dec_ || index < 0 || index >= int(video_.size())) return false;
    int cw = (w_ + 3) / 4;
    while (decoded_ < index) {
        decoded_++;
        const Chunk& c = video_[size_t(decoded_)];
        if (c.size == 0) continue;  // dropped frame: the picture stays
        // the decoder may read a few bytes past the end of a frame
        std::vector<uint8_t> buf(data_.begin() + long(c.offset), data_.begin() + long(c.offset + c.size));
        buf.resize(buf.size() + 16, 0);
        int r = indeo5_decode(dec_, buf.data(), int(c.size), y_.data(), w_, u_.data(), v_.data(), cw);
        if (r > 0) haveFrame_ = true;
    }
    if (!haveFrame_) return false;
    toRgba(out);
    return true;
}

void Movie::toRgba(Image& out) const {
    out.w = w_, out.h = h_;
    out.rgba.resize(size_t(w_) * h_ * 4);
    out.hasAlpha = false;
    int cw = (w_ + 3) / 4;
    for (int y = 0; y < h_; y++)
        for (int x = 0; x < w_; x++) {
            // YVU9: one chroma sample per 4x4; BT.601 limited range
            int Y = y_[size_t(y) * w_ + x] - 16;
            int U = u_[size_t(y / 4) * cw + x / 4] - 128, V = v_[size_t(y / 4) * cw + x / 4] - 128;
            int C = 298 * Y;
            uint8_t* o = &out.rgba[(size_t(y) * w_ + x) * 4];
            o[0] = uint8_t(std::clamp((C + 409 * V + 128) >> 8, 0, 255));
            o[1] = uint8_t(std::clamp((C - 100 * U - 208 * V + 128) >> 8, 0, 255));
            o[2] = uint8_t(std::clamp((C + 516 * U + 128) >> 8, 0, 255));
            o[3] = 255;
        }
}

}  // namespace hg2
