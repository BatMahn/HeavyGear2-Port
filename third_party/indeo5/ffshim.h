/*
 * Minimal stand-ins for the parts of libavutil / libavcodec used by FFmpeg's Indeo 5 decoder
 * (ivi.c, ivi_dsp.c, indeo5.c), so those files build on their own.
 *
 * This file is part of the HG2 port's copy of the FFmpeg Indeo decoder and is distributed under
 * the GNU Lesser General Public License, version 2.1 or later (see COPYING.LGPLv2.1).
 */
#ifndef HG2_FFSHIM_H
#define HG2_FFSHIM_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define av_cold
#define av_unused
#define av_always_inline inline
#define attribute_unused

#define AVERROR(e) (-(e))
#define AVERROR_INVALIDDATA (-0x41444E49)
#define AVERROR_PATCHWELCOME (-0x45574150)
#ifndef ENOMEM
#define ENOMEM 12
#endif
#ifndef EINVAL
#define EINVAL 22
#endif

#define AV_LOG_ERROR 16
#define AV_LOG_DEBUG 48
#define av_log(ctx, level, ...) ((void)0)
#define ff_dlog(ctx, ...) ((void)0)
#define avpriv_request_sample(ctx, ...) ((void)0)
#define avpriv_report_missing_feature(ctx, ...) ((void)0)
#define av_assert0(c) ((void)0)

#define FFMIN(a, b) ((a) > (b) ? (b) : (a))
#define FFMAX(a, b) ((a) > (b) ? (a) : (b))
#define FFSIGN(a) ((a) > 0 ? 1 : -1)
#define FFALIGN(x, a) (((x) + (a) - 1) & ~((a) - 1))
#define FFSWAP(type, a, b) do { type SWAP_tmp = b; b = a; a = SWAP_tmp; } while (0)

static inline int av_clip(int a, int amin, int amax) { return a < amin ? amin : a > amax ? amax : a; }
static inline uint8_t av_clip_uint8(int a) { return (uint8_t)(a < 0 ? 0 : a > 255 ? 255 : a); }
static inline unsigned av_clip_uintp2(int a, int p) { return a & ~((1 << p) - 1) ? (~a >> 31) & ((1 << p) - 1) : a; }

static inline void *av_mallocz(size_t n) { return calloc(1, n ? n : 1); }
static inline void *av_calloc(size_t n, size_t s) { return calloc(n ? n : 1, s ? s : 1); }
static inline void av_freep(void *p) { void **pp = (void **)p; free(*pp); *pp = NULL; }

/* ---------------------------------------------------------------- codec context stand-ins */
enum { AV_CODEC_ID_INDEO4 = 1, AV_CODEC_ID_INDEO5 = 2 };
enum { AV_PIX_FMT_YUV410P = 1 };
typedef struct AVCodecContext {
    int width, height;
    int codec_id;
    int pix_fmt;
    long long max_pixels;
    void *priv_data;
} AVCodecContext;
typedef struct AVFrame {
    uint8_t *data[3];
    int linesize[3];
} AVFrame;
typedef struct AVPacket {
    const uint8_t *data;
    int size;
} AVPacket;
static inline int av_image_check_size2(unsigned w, unsigned h, long long max, int fmt, int off, void *ctx)
{
    (void)max, (void)fmt, (void)off, (void)ctx;
    return w > 0 && h > 0 && w <= 4096 && h <= 4096 ? 0 : -1;
}
static inline int ff_set_dimensions(AVCodecContext *c, int w, int h) { c->width = w, c->height = h; return 0; }
static inline int ff_get_buffer(AVCodecContext *c, AVFrame *f, int flags) { (void)c, (void)flags; return f->data[0] ? 0 : -1; }
static inline void av_frame_unref(AVFrame *f) { (void)f; }
static inline void av_frame_move_ref(AVFrame *d, AVFrame *s) { (void)d, (void)s; }
static inline void av_frame_free(AVFrame **f) { (void)f; }

/* ---------------------------------------------------------------- little-endian bit reader */
typedef struct GetBitContext {
    const uint8_t *buffer;
    int index;
    int size_in_bits;
} GetBitContext;

static inline int init_get_bits8(GetBitContext *s, const uint8_t *buf, int bytes)
{
    s->buffer = buf, s->index = 0, s->size_in_bits = bytes * 8;
    return 0;
}
static inline unsigned show_bits(GetBitContext *s, int n)
{
    /* reads past the end return zero bits */
    unsigned v = 0;
    int byte = s->index >> 3, sh = s->index & 7, bytes = (s->size_in_bits >> 3);
    for (int i = 0; i < 4; i++)
        if (byte + i < bytes) v |= (unsigned)s->buffer[byte + i] << (8 * i);
    v >>= sh;
    if (byte + 4 < bytes && sh) v |= (unsigned)s->buffer[byte + 4] << (32 - sh);
    return n >= 32 ? v : v & ((1u << n) - 1);
}
static inline void skip_bits(GetBitContext *s, int n) { s->index += n; }
static inline void skip_bits_long(GetBitContext *s, int n) { s->index += n; }
static inline unsigned get_bits(GetBitContext *s, int n)
{
    unsigned v = show_bits(s, n);
    s->index += n;
    return v;
}
static inline unsigned get_bits1(GetBitContext *s) { return get_bits(s, 1); }
static inline unsigned get_bits_long(GetBitContext *s, int n)
{
    if (n <= 24) return get_bits(s, n);
    unsigned lo = get_bits(s, 16);
    return lo | (get_bits(s, n - 16) << 16);
}
static inline int get_bits_count(const GetBitContext *s) { return s->index; }
static inline int get_bits_left(const GetBitContext *s) { return s->size_in_bits - s->index; }
static inline void align_get_bits(GetBitContext *s) { s->index = (s->index + 7) & ~7; }

/* ---------------------------------------------------------------- VLC: lookup on the next nb bits (LE order) */
typedef struct VLCElem {
    int16_t sym, len;
} VLCElem;
typedef struct VLC {
    VLCElem *table;
    int table_allocated;
    int bits;
    int owned;
} VLC;
#define VLC_INIT_USE_STATIC 1
#define VLC_INIT_OUTPUT_LE 8

/* codes are given MSB-first; the stream carries them first bit lowest (Intel's LE packing) */
static inline int vlc_init(VLC *vlc, int nb_bits, int nb_codes, const uint8_t *bits, int bw, int bs,
                           const uint16_t *codes, int cw, int cs, int flags)
{
    (void)bw, (void)bs, (void)cw, (void)cs;
    int size = 1 << nb_bits;
    if (!(flags & VLC_INIT_USE_STATIC) || !vlc->table) {
        vlc->table = (VLCElem *)calloc((size_t)size, sizeof(VLCElem));
        vlc->owned = 1;
        if (!vlc->table) return AVERROR(ENOMEM);
    }
    vlc->bits = nb_bits;
    for (int i = 0; i < size; i++) vlc->table[i].sym = -1, vlc->table[i].len = 0;
    for (int s = 0; s < nb_codes; s++) {
        int l = bits[s];
        if (l <= 0 || l > nb_bits) continue;
        unsigned rev = 0;
        for (int k = 0; k < l; k++) rev |= ((codes[s] >> (l - 1 - k)) & 1u) << k;
        for (unsigned f = 0; f < (1u << (nb_bits - l)); f++) {
            VLCElem *e = &vlc->table[rev | (f << l)];
            e->sym = (int16_t)s, e->len = (int16_t)l;
        }
    }
    return 0;
}
static inline void ff_vlc_free(VLC *vlc)
{
    if (vlc->owned) free(vlc->table);
    vlc->table = NULL, vlc->owned = 0;
}
static inline int get_vlc2(GetBitContext *s, const VLCElem *table, int bits, int max_depth)
{
    (void)max_depth;
    const VLCElem *e = &table[show_bits(s, bits)];
    if (e->len <= 0) {
        s->index += bits;
        return -1;
    }
    s->index += e->len;
    return e->sym;
}

extern const uint8_t ff_zigzag_direct[64];
typedef int AVOnce;
#define AV_ONCE_INIT 0
#define ff_thread_once(once, fn) do { if (!*(once)) { *(once) = 1; fn(); } } while (0)
int ff_indeo5_decode_init(AVCodecContext *avctx);

#endif
