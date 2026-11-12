/* C interface to the FFmpeg-derived Indeo 5 decoder. LGPL-2.1-or-later, see COPYING.LGPLv2.1. */
#ifndef HG2_INDEO5DEC_H
#define HG2_INDEO5DEC_H
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
typedef struct Indeo5Decoder Indeo5Decoder;
Indeo5Decoder *indeo5_open(int width, int height);
/* Decodes one frame into YUV 4:1:0 planes (Y width x height; U, V (width+3)/4 x (height+3)/4).
   Returns 1 when a picture was produced, 0 for a frame without one (null frame), negative on error. */
int indeo5_decode(Indeo5Decoder *d, const uint8_t *data, int size, uint8_t *y, int ypitch, uint8_t *u, uint8_t *v, int cpitch);
void indeo5_close(Indeo5Decoder *d);
#ifdef __cplusplus
}
#endif
#endif
