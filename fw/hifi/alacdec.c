/* ALAC (Apple Lossless) decoder for core B (ETUDE §57): Apple's reference decoder
 * (third_party/alac) rewritten in one C file for the Cortex-M3:
 *   - leading zeros counted by the clz instruction (the reference loops over the 32 bits, twice
 *     per sample in the Rice decoder: "a big performance win", says its own comment);
 *   - the bitstream read as big-endian words (unaligned ldr + rev), not byte by byte;
 *   - the output written straight in the format of core B's ring (16-bit: packed L | R << 16;
 *     20 and 24-bit: one word per sample, data in the high bits), without the packed 3-byte
 *     samples of the reference.
 * Same algorithms otherwise (adaptive Golomb decoding, adaptive FIR predictor, stereo matrix):
 * the output is bit-exact with the reference. Mono or stereo (one SCE or CPE element), 16, 20
 * or 24 bits.
 *
 * Derived from the ALAC decoder of Apple Inc. (Copyright (c) 2011 Apple Inc. All rights
 * reserved), licensed under the Apache License, Version 2.0 (third_party/alac/LICENSE); modified
 * as described above. */
#include <stdint.h>
#include <string.h>
#include "alacdec.h"

/* ---- bits ---- */
static inline uint32_t clz32(uint32_t x)
{
    return x ? (uint32_t)__builtin_clz(x) : 32;     /* one clz on ARM, 32 for 0 */
}

static inline uint32_t load_be32(const uint8_t *p)
{
    uint32_t v;
    __builtin_memcpy(&v, p, 4);                     /* unaligned ldr on the M3 (inlined even with -fno-builtin) */
    return __builtin_bswap32(v);
}

struct bits { const uint8_t *buf; uint32_t pos, end; };    /* positions in bits */

static inline uint32_t peek(const struct bits *b)   /* 25 valid bits at least, left-justified */
{
    return load_be32(b->buf + (b->pos >> 3)) << (b->pos & 7);
}

static inline uint32_t get(struct bits *b, uint32_t n)     /* 1 <= n <= 24 */
{
    uint32_t v = peek(b) >> (32 - n);
    b->pos += n;
    return v;
}

static inline uint32_t get32(struct bits *b)
{
    uint32_t h = get(b, 16);
    return h << 16 | get(b, 16);
}

/* ---- adaptive Golomb (the reference's ag_dec.c) ---- */
#define QBSHIFT     9
#define QB          (1 << QBSHIFT)
#define MMULSHIFT   2
#define MDENSHIFT   (QBSHIFT - MMULSHIFT - 1)
#define MOFF        (1 << (MDENSHIFT - 2))
#define BITOFF      24
#define MAX_PREFIX_16       9
#define MAX_PREFIX_32       9
#define MAX_DATATYPE_BITS_16 16
#define N_MAX_MEAN_CLAMP    0xffff
#define N_MEAN_CLAMP_VAL    0xffff

/* maxbits up to 32: read across five bytes */
static inline uint32_t stream_bits(const uint8_t *in, uint32_t pos, uint32_t n)
{
    uint32_t hi = load_be32(in + (pos >> 3)), sh = pos & 7;
    uint64_t v = ((uint64_t)hi << 8 | in[(pos >> 3) + 4]) << (sh + 24);
    return (uint32_t)(v >> (64 - n));
}

__attribute__((noinline)) static int dyn_decomp(struct bits *b, int32_t *pc, uint32_t num, uint32_t maxbits,
                      uint32_t mb0, uint32_t pb, uint32_t kb)
{
    const uint8_t *in = b->buf;
    uint32_t pos = b->pos, maxpos = b->end, wb = (1u << kb) - 1;
    uint32_t mb = mb0, c = 0, zmode = 0;
    while (c < num) {
        if (pos >= maxpos)
            return -1;
        uint32_t k = 31 - clz32((mb >> QBSHIFT) + 3);
        if (k > kb)
            k = kb;
        uint32_t m = (1u << k) - 1;
        /* dyn_get_32bit */
        uint32_t s = load_be32(in + (pos >> 3)) << (pos & 7), n = clz32(~s);
        if (n >= MAX_PREFIX_32) {
            n = stream_bits(in, pos + MAX_PREFIX_32, maxbits);
            pos += MAX_PREFIX_32 + maxbits;
        } else {
            pos += n + 1;
            if (k != 1) {
                uint32_t v = (s << (n + 1)) >> (32 - k);
                pos += k - 1;
                n *= m;
                if (v >= 2) {
                    n += v - 1;
                    pos++;
                }
            }
        }
        uint32_t nd = n + zmode;
        int32_t sgn = -(int32_t)(nd & 1) | 1;
        *pc++ = (int32_t)((nd + 1) >> 1) * sgn;     /* the least significant bit is the sign */
        c++;
        mb = pb * (n + zmode) + mb - ((pb * mb) >> QBSHIFT);
        if (n > N_MAX_MEAN_CLAMP)
            mb = N_MEAN_CLAMP_VAL;
        zmode = 0;
        if ((mb << MMULSHIFT) < QB && c < num) {    /* a run of zeros */
            zmode = 1;
            k = clz32(mb) - BITOFF + ((mb + MOFF) >> MDENSHIFT);
            uint32_t mz = ((1u << k) - 1) & wb;
            /* dyn_get */
            s = load_be32(in + (pos >> 3)) << (pos & 7);
            uint32_t pre = clz32(~s), r;
            if (pre >= MAX_PREFIX_16) {
                pos += MAX_PREFIX_16;
                r = (s << MAX_PREFIX_16) >> (32 - MAX_DATATYPE_BITS_16);
                pos += MAX_DATATYPE_BITS_16;
            } else {
                pos += pre + 1;
                uint32_t v = k ? (s << (pre + 1)) >> (32 - k) : 0;
                pos += k;
                r = pre * mz + v - 1;
                if (v < 2) {
                    r -= v - 1;
                    pos--;
                }
            }
            if (c + r > num)
                return -1;
            memset(pc, 0, r * 4);
            pc += r;
            c += r;
            if (r >= 65535)
                zmode = 0;
            mb = 0;
        }
    }
    b->pos = pos;
    return pos > maxpos ? -1 : 0;
}

/* ---- adaptive FIR predictor (the reference's dp_dec.c) ---- */
static inline int32_t sign_of(int32_t i)
{
    return (int32_t)((uint32_t)-i >> 31) | (i >> 31);
}

/* The predictor for a fixed order n (1 to 8), the loops unrolled by the compiler and the
 * coefficients kept in registers: the reference's arithmetic, in its order (16-bit coefficients,
 * the update from the oldest tap to the newest while the error keeps its sign). Called after the
 * first n + 1 samples. */
static inline __attribute__((always_inline)) void pred_fixed(const int32_t *pc, int32_t *out,
        uint32_t num, int16_t *coefs, const int n, uint32_t cs, uint32_t denshift)
{
    int16_t a[8];
    int32_t denhalf = 1 << (denshift - 1);
    for (int i = 0; i < n; i++)
        a[i] = coefs[i];
    const int32_t *po = out + n;                /* po[-i]: the sample i + 1 before */
    for (uint32_t j = (uint32_t)n + 1; j < num; j++, po++) {
        int32_t top = po[-n], sum = denhalf;
        for (int i = 0; i < n; i++)
            sum -= a[i] * (top - po[-i]);
        int32_t del = pc[j], del0 = del, sg = sign_of(del);
        del += top + (sum >> denshift);
        out[j] = (del << cs) >> cs;
        if (sg > 0) {
            for (int i = n - 1; i > 0; i--) {
                int32_t bi = top - po[-i], s = sign_of(bi);
                a[i] = (int16_t)(a[i] - s);
                del0 -= (n - i) * ((s * bi) >> denshift);
                if (del0 <= 0)
                    goto next;
            }
            a[0] = (int16_t)(a[0] - sign_of(top - po[0]));
        } else if (sg < 0) {
            for (int i = n - 1; i > 0; i--) {
                int32_t bi = top - po[-i], s = -sign_of(bi);
                a[i] = (int16_t)(a[i] - s);
                del0 -= (n - i) * ((s * bi) >> denshift);
                if (del0 >= 0)
                    goto next;
            }
            a[0] = (int16_t)(a[0] + sign_of(top - po[0]));
        }
next:   ;
    }
    for (int i = 0; i < n; i++)
        coefs[i] = a[i];
}

__attribute__((noinline)) static void unpc_block(const int32_t *pc, int32_t *out, uint32_t num, int16_t *coefs,
                       uint32_t act, uint32_t chanbits, uint32_t denshift)
{
    uint32_t cs = 32 - chanbits;
    int32_t denhalf = 1 << (denshift - 1);
    out[0] = pc[0];
    if (act == 0) {
        if (num > 1 && pc != out)
            memcpy(&out[1], &pc[1], (num - 1) * 4);
        return;
    }
    if (act == 31) {                            /* first difference, in place */
        int32_t prev = out[0];
        for (uint32_t j = 1; j < num; j++) {
            int32_t d = pc[j] + prev;
            prev = (d << cs) >> cs;
            out[j] = prev;
        }
        return;
    }
    for (uint32_t j = 1; j <= act && j < num; j++) {
        int32_t d = pc[j] + out[j - 1];
        out[j] = (d << cs) >> cs;
    }
    uint32_t lim = act + 1;
    switch (act) {                              /* the common orders unrolled (ffmpeg: 4 to 6, */
    case 1: pred_fixed(pc, out, num, coefs, 1, cs, denshift); return;   /* Apple's encoder: */
    case 2: pred_fixed(pc, out, num, coefs, 2, cs, denshift); return;   /* 4 and 8) */
    case 3: pred_fixed(pc, out, num, coefs, 3, cs, denshift); return;
    case 4: pred_fixed(pc, out, num, coefs, 4, cs, denshift); return;
    case 5: pred_fixed(pc, out, num, coefs, 5, cs, denshift); return;
    case 6: pred_fixed(pc, out, num, coefs, 6, cs, denshift); return;
    case 7: pred_fixed(pc, out, num, coefs, 7, cs, denshift); return;
    case 8: pred_fixed(pc, out, num, coefs, 8, cs, denshift); return;
    }
    /* any other order: the reference's general loop */
    int16_t a[32];
    for (uint32_t i = 0; i < act; i++)
        a[i] = coefs[i];
    for (uint32_t j = lim; j < num; j++) {
        const int32_t *po = out + j - 1;
        int32_t top = out[j - lim], sum = 0;
        for (uint32_t k = 0; k < act; k++)
            sum += a[k] * (po[-(int32_t)k] - top);
        int32_t del = pc[j], del0 = del, sg = sign_of(del);
        del += top + ((sum + denhalf) >> denshift);
        out[j] = (del << cs) >> cs;
        if (sg > 0) {
            for (int32_t k = (int32_t)act - 1; k >= 0; k--) {
                int32_t dd = top - po[-k], s = sign_of(dd);
                a[k] = (int16_t)(a[k] - s);
                del0 -= ((int32_t)act - k) * ((s * dd) >> denshift);
                if (del0 <= 0)
                    break;
            }
        } else if (sg < 0) {
            for (int32_t k = (int32_t)act - 1; k >= 0; k--) {
                int32_t dd = top - po[-k], s = sign_of(dd);
                a[k] = (int16_t)(a[k] + s);
                del0 -= ((int32_t)act - k) * ((-s * dd) >> denshift);
                if (del0 >= 0)
                    break;
            }
        }
    }
    for (uint32_t i = 0; i < act; i++)
        coefs[i] = a[i];
}

/* ---- set-up ---- */
int alac_init(struct alac *a, const uint8_t *c, uint32_t n, void *(*alloc)(uint32_t))
{
    if (n >= 12 && !memcmp(c + 4, "frma", 4))
        c += 12, n -= 12;
    if (n >= 12 && !memcmp(c + 4, "alac", 4))
        c += 12, n -= 12;
    if (n < 24)
        return -1;
    a->frame_len = load_be32(c);
    a->bits = c[5];
    a->pb = c[6];
    a->mb = c[7];
    a->kb = c[8];
    a->channels = c[9];
    a->max_run = (uint32_t)c[10] << 8 | c[11];
    a->max_frame_bytes = load_be32(c + 12);
    a->rate = load_be32(c + 20);
    if (!a->frame_len || a->frame_len > 16384 || a->channels < 1 || a->channels > 2 ||
        (a->bits != 16 && a->bits != 20 && a->bits != 24) || a->kb > 31)
        return -1;
    a->pred = alloc(a->frame_len * 4 + 8);
    a->u = alloc(a->frame_len * 4);
    a->v = alloc(a->frame_len * 4);
    return a->pred && a->u && a->v ? 0 : -1;
}

/* ---- one element (SCE or CPE) into u (and v) ---- */
struct chan { uint32_t mode, den, pbf, num; int16_t coefs[32]; };

static void chan_header(struct bits *b, struct chan *ch)
{
    uint32_t h = get(b, 8);
    ch->mode = h >> 4;
    ch->den = h & 15;
    h = get(b, 8);
    ch->pbf = h >> 5;
    ch->num = h & 31;
    for (uint32_t i = 0; i < ch->num; i++)
        ch->coefs[i] = (int16_t)get(b, 16);
}

static int chan_decode(struct alac *a, struct bits *b, struct chan *ch, int32_t *out,
                       uint32_t num, uint32_t chanbits)
{
    if (dyn_decomp(b, a->pred, num, chanbits, a->mb, (a->pb * ch->pbf) / 4, a->kb))
        return -1;
    if (ch->mode)
        unpc_block(a->pred, a->pred, num, 0, 31, chanbits, 0);
    unpc_block(a->pred, out, num, ch->coefs, ch->num, chanbits, ch->den);
    return 0;
}

static int32_t raw_sample(struct bits *b, uint32_t chanbits)
{
    uint32_t sh = 32 - chanbits;
    if (chanbits <= 16)
        return (int32_t)(get(b, chanbits) << sh) >> sh;
    int32_t v = (int32_t)(get(b, 16) << 16) >> sh;
    return v | (int32_t)get(b, chanbits - 16);
}

int alac_decode(struct alac *a, const uint8_t *pkt, uint32_t n, uint32_t *out, uint32_t *frames)
{
    struct bits b = { pkt, 0, n * 8 };
    uint32_t num = a->frame_len, chans = 0;
    *frames = 0;
    while (chans < a->channels) {
        if (b.pos + 3 > b.end)
            return -1;
        uint32_t tag = get(&b, 3);
        if (tag == 6) {                         /* FIL: fill bytes */
            uint32_t cnt = get(&b, 4);
            if (cnt == 15)
                cnt += get(&b, 8) - 1;
            b.pos += cnt * 8;
            continue;
        }
        if (tag == 4) {                         /* DSE: data stream, skipped */
            get(&b, 4);
            uint32_t align = get(&b, 1), cnt = get(&b, 8);
            if (cnt == 255)
                cnt += get(&b, 8);
            if (align)
                b.pos = (b.pos + 7) & ~7u;
            b.pos += cnt * 8;
            continue;
        }
        if (tag == 7)                           /* END */
            break;
        if ((tag != 0 && tag != 1 && tag != 3) || (tag == 1) != (a->channels == 2))
            return -1;                          /* only one SCE (or LFE) or one CPE */
        uint32_t stereo = tag == 1;
        get(&b, 4);                             /* element instance tag */
        if (get(&b, 12))
            return -1;                          /* unused header bits */
        uint32_t h = get(&b, 4), partial = h >> 3, shifted = (h >> 1) & 3, escape = h & 1;
        if (shifted == 3)
            return -1;
        if (partial) {
            num = get32(&b);
            if (num > a->frame_len)
                return -1;
        }
        uint32_t chanbits = a->bits - shifted * 8 + stereo;
        int32_t mixbits = 0, mixres = 0;
        uint16_t *sh = (uint16_t *)a->pred;     /* the shift buffer: after the predictor's use */
        if (!escape) {
            struct chan cu, cv;
            mixbits = (int32_t)get(&b, 8);
            mixres = (int8_t)get(&b, 8);
            chan_header(&b, &cu);
            if (stereo)
                chan_header(&b, &cv);
            struct bits sb = b;
            b.pos += shifted * 8 * (stereo + 1) * num;
            if (chan_decode(a, &b, &cu, a->u, num, chanbits) ||
                (stereo && chan_decode(a, &b, &cv, a->v, num, chanbits)))
                return -1;
            if (shifted) {
                if (sb.pos + shifted * 8 * (stereo + 1) * num > b.end)
                    return -1;
                for (uint32_t i = 0; i < num * (stereo + 1); i++)
                    sh[i] = (uint16_t)get(&sb, shifted * 8);
            }
        } else {                                /* uncompressed */
            chanbits = a->bits;
            if (b.pos + chanbits * (stereo + 1) * num > b.end)
                return -1;
            for (uint32_t i = 0; i < num; i++) {
                a->u[i] = raw_sample(&b, chanbits);
                if (stereo)
                    a->v[i] = raw_sample(&b, chanbits);
            }
            shifted = 0;
        }
        if (b.pos > b.end)
            return -1;
        /* into the ring's format */
        const int32_t *u = a->u, *v = a->v;
        uint32_t s8 = shifted * 8, sstep = stereo + 1;
        if (a->bits == 16) {
            if (!stereo)
                for (uint32_t i = 0; i < num; i++)
                    out[i] = (uint16_t)u[i] | (uint32_t)u[i] << 16;
            else if (mixres)
                for (uint32_t i = 0; i < num; i++) {
                    int32_t l = u[i] + v[i] - ((mixres * v[i]) >> mixbits), r = l - v[i];
                    out[i] = (uint16_t)l | (uint32_t)r << 16;
                }
            else
                for (uint32_t i = 0; i < num; i++)
                    out[i] = (uint16_t)u[i] | (uint32_t)v[i] << 16;
        } else {
            uint32_t up = a->bits == 20 ? 12 : 8;   /* 20-bit: the reference's << 4, then << 8 */
            for (uint32_t i = 0; i < num; i++) {
                int32_t l = u[i], r = l;
                if (stereo) {
                    r = v[i];
                    if (mixres) {
                        l = u[i] + v[i] - ((mixres * v[i]) >> mixbits);
                        r = l - v[i];
                    }
                }
                if (s8) {
                    l = (int32_t)((uint32_t)l << s8 | sh[i * sstep]);
                    r = stereo ? (int32_t)((uint32_t)r << s8 | sh[i * sstep + 1]) : l;
                }
                out[2 * i] = (uint32_t)l << up;
                out[2 * i + 1] = (uint32_t)r << up;
            }
        }
        chans += stereo + 1;
        *frames = num;
    }
    return chans ? 0 : -1;
}
