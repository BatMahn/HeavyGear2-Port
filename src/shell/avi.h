// AVI soundtracks: the briefing "SATCOM intel" movies (Shell\movies\*.avi) carry Indeo 5 video, which is not
// decoded, and uncompressed PCM narration, which is played over the briefing's still picture.
#pragma once
#include <SDL3/SDL.h>

#include <vector>

#include "../core/fs.h"

namespace hg2 {

bool aviAudio(const Bytes& avi, SDL_AudioSpec& spec, std::vector<uint8_t>& pcm);

}  // namespace hg2
