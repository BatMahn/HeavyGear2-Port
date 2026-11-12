/* Tables the Indeo decoder borrows from libavcodec (mathtables.c). LGPL-2.1-or-later, see COPYING.LGPLv2.1. */
#include "ffshim.h"
const uint8_t ff_zigzag_direct[64] = {
    0,   1,  8, 16,  9,  2,  3, 10,
    17, 24, 32, 25, 18, 11,  4,  5,
    12, 19, 26, 33, 40, 48, 41, 34,
    27, 20, 13,  6,  7, 14, 21, 28,
    35, 42, 49, 56, 57, 50, 43, 36,
    29, 22, 15, 23, 30, 37, 44, 51,
    58, 59, 52, 45, 38, 31, 39, 46,
    53, 60, 61, 54, 47, 55, 62, 63
};

#include "ivi.h"
#include "indeo5dec.h"

struct Indeo5Decoder {
    AVCodecContext avctx;
    IVI45DecContext ctx;
};

Indeo5Decoder *indeo5_open(int width, int height)
{
    Indeo5Decoder *d = (Indeo5Decoder *)calloc(1, sizeof(Indeo5Decoder));
    if (!d) return NULL;
    d->avctx.width = width, d->avctx.height = height;
    d->avctx.codec_id = AV_CODEC_ID_INDEO5;
    d->avctx.priv_data = &d->ctx;
    if (ff_indeo5_decode_init(&d->avctx) < 0) {
        ff_ivi_decode_close(&d->avctx);
        free(d);
        return NULL;
    }
    return d;
}

int indeo5_decode(Indeo5Decoder *d, const uint8_t *data, int size, uint8_t *y, int ypitch, uint8_t *u, uint8_t *v, int cpitch)
{
    AVFrame f;
    AVPacket p;
    int got = 0, r;
    f.data[0] = y, f.data[1] = u, f.data[2] = v;
    f.linesize[0] = ypitch, f.linesize[1] = f.linesize[2] = cpitch;
    p.data = data, p.size = size;
    r = ff_ivi_decode_frame(&d->avctx, &f, &got, &p);
    if (r < 0) return r;
    return got;
}

void indeo5_close(Indeo5Decoder *d)
{
    if (!d) return;
    ff_ivi_decode_close(&d->avctx);
    free(d);
}
