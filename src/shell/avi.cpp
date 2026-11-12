#include "avi.h"

#include <cstring>

namespace hg2 {

namespace {
uint32_t rd32(const uint8_t* p) { return uint32_t(p[0]) | uint32_t(p[1]) << 8 | uint32_t(p[2]) << 16 | uint32_t(p[3]) << 24; }
uint16_t rd16(const uint8_t* p) { return uint16_t(p[0] | p[1] << 8); }

struct Walker {
    const Bytes& b;
    int stream = -1, audioStream = -1;
    char lastType[5] = {0};
    uint16_t format = 0, channels = 0, bits = 0;
    uint32_t rate = 0;
    std::vector<uint8_t>* pcm;
    void walk(size_t p, size_t end, int depth) {
        while (p + 8 <= end && depth < 8) {
            uint32_t id = rd32(&b[p]), sz = rd32(&b[p + 4]);
            size_t body = p + 8, next = body + sz + (sz & 1);
            if (body + sz > b.size()) break;
            char fcc[5] = {char(id), char(id >> 8), char(id >> 16), char(id >> 24), 0};
            if (!strcmp(fcc, "LIST") || !strcmp(fcc, "RIFF")) {
                if (sz >= 4) {
                    if (!memcmp(&b[body], "strl", 4)) stream++;
                    walk(body + 4, body + sz, depth + 1);
                }
            } else if (!strcmp(fcc, "strh") && sz >= 8) {
                memcpy(lastType, &b[body], 4);
                if (!memcmp(lastType, "auds", 4) && audioStream < 0) audioStream = stream;
            } else if (!strcmp(fcc, "strf") && stream == audioStream && sz >= 16 && format == 0) {
                format = rd16(&b[body]), channels = rd16(&b[body + 2]), rate = rd32(&b[body + 4]), bits = rd16(&b[body + 14]);
            } else if (audioStream >= 0 && fcc[2] == 'w' && fcc[3] == 'b' && (fcc[0] - '0') * 10 + (fcc[1] - '0') == audioStream) {
                pcm->insert(pcm->end(), b.begin() + long(body), b.begin() + long(body + sz));
            }
            p = next;
        }
    }
};
}  // namespace

bool aviAudio(const Bytes& avi, SDL_AudioSpec& spec, std::vector<uint8_t>& pcm) {
    if (avi.size() < 12 || memcmp(avi.data(), "RIFF", 4) != 0 || memcmp(avi.data() + 8, "AVI ", 4) != 0) return false;
    Walker w{avi};
    w.pcm = &pcm;
    pcm.clear();
    w.walk(12, avi.size(), 0);
    if (w.format != 1 || !w.channels || !w.rate || pcm.empty()) return false;  // PCM only
    spec.format = w.bits == 8 ? SDL_AUDIO_U8 : SDL_AUDIO_S16LE;
    spec.channels = w.channels;
    spec.freq = int(w.rate);
    return true;
}

}  // namespace hg2
