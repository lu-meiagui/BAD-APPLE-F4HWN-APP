#ifndef BAD_APPLE_RC_H
#define BAD_APPLE_RC_H

#include <stdint.h>
#include <stdbool.h>

#define BA_LOGICAL_W   32u
#define BA_LOGICAL_H   16u
#define BA_BITS_PER_FRAME (BA_LOGICAL_W * BA_LOGICAL_H)
#define BA_FRAME_BYTES (BA_BITS_PER_FRAME / 8u)

#define RC_TOP    (1u << 24)
#define RC_BOTTOM (1u << 16)
#define N_CTX     64u

#define PROB_BITS 12u
#define PROB_MAX  (1u << PROB_BITS)
#define PROB_INIT 2048u
#define MOVE_BITS 5u

typedef void (*ba_draw_block_fn)(uint16_t bx, uint16_t by, bool on, void *user);

typedef struct {
    const uint8_t *data;
    uint32_t data_len;
    uint32_t pos;
    uint32_t low, range, code;
} rc_dec_t;

typedef struct {
    rc_dec_t rc;
    uint16_t prob[N_CTX];
    uint8_t  framebuf[BA_FRAME_BYTES];
    uint8_t  diff_buf[2][BA_FRAME_BYTES];
    uint8_t  cur_idx;
    bool     have_prev;
} ba_decoder_t;

static inline bool ba__get(const uint8_t *buf, int16_t x, int16_t y) {
    if (x < 0 || x >= (int16_t)BA_LOGICAL_W || y < 0 || y >= (int16_t)BA_LOGICAL_H)
        return false;
    uint16_t pos = (uint16_t)(y * BA_LOGICAL_W + x);
    return (buf[pos >> 3] >> (7 - (pos & 7))) & 1u;
}

static inline bool ba__get_unchecked(const uint8_t *buf, uint16_t x, uint16_t y) {
    uint16_t pos = (uint16_t)(y * BA_LOGICAL_W + x);
    return (buf[pos >> 3] >> (7 - (pos & 7))) & 1u;
}

static inline void ba__set(uint8_t *buf, uint16_t pos, bool on) {
    uint8_t mask = (uint8_t)(0x80u >> (pos & 7));
    if (on) buf[pos >> 3] |= mask; else buf[pos >> 3] &= (uint8_t)~mask;
}

static inline void ba__xor(uint8_t *buf, uint16_t pos, bool flip) {
    if (flip) buf[pos >> 3] ^= (uint8_t)(0x80u >> (pos & 7));
}

static inline uint8_t rc__byte(rc_dec_t *rc) {
    return (rc->pos < rc->data_len) ? rc->data[rc->pos++] : 0;
}

static inline void rc__normalize(rc_dec_t *rc) {
    for (;;) {
        if ((rc->low ^ (rc->low + rc->range)) < RC_TOP) {
        } else if (rc->range < RC_BOTTOM) {
            rc->range = (0u - rc->low) & (RC_BOTTOM - 1u);
        } else {
            break;
        }
        rc->code  = (rc->code  << 8) | rc__byte(rc);
        rc->low   = (rc->low   << 8);
        rc->range = (rc->range << 8);
    }
}

static inline void rc_init(rc_dec_t *rc, const uint8_t *data, uint32_t len) {
    rc->data = data; rc->data_len = len; rc->pos = 0;
    rc->low = 0; rc->range = 0xFFFFFFFFu; rc->code = 0;
    for (int i = 0; i < 4; i++) rc->code = (rc->code << 8) | rc__byte(rc);
}

static inline uint8_t rc_decode_bit(rc_dec_t *rc, uint16_t *prob) {
    uint32_t p = *prob;
    uint32_t bound = (rc->range >> PROB_BITS) * p;
    uint32_t v = rc->code - rc->low;
    uint8_t bit;
    if (v < bound) {
        bit = 0;
        rc->range = bound;
        p += (PROB_MAX - p) >> MOVE_BITS;
    } else {
        bit = 1;
        rc->low += bound;
        rc->range -= bound;
        p -= p >> MOVE_BITS;
    }
    *prob = (uint16_t)p;
    rc__normalize(rc);
    return bit;
}

static inline uint8_t compute_context(const uint8_t *cur_diff, const uint8_t *prev_diff,
                                       bool have_prev, int16_t x, int16_t y) {
    uint8_t spatial = (uint8_t)(
        (ba__get(cur_diff, x - 1, y)     << 5) |
        (ba__get(cur_diff, x,     y - 1) << 4) |
        (ba__get(cur_diff, x - 1, y - 1) << 3) |
        (ba__get(cur_diff, x + 1, y - 1) << 2));
    uint8_t exact = 0, nearby = 0;
    if (have_prev) {
        exact  = ba__get_unchecked(prev_diff, (uint16_t)x, (uint16_t)y) ? 2u : 0u;
        nearby = (ba__get(prev_diff, x - 1, y) || ba__get(prev_diff, x + 1, y) ||
                  ba__get(prev_diff, x, y - 1) || ba__get(prev_diff, x, y + 1)) ? 1u : 0u;
    }
    return (uint8_t)(spatial | exact | nearby);
}

static inline void ba_init(ba_decoder_t *ctx, const uint8_t *data, uint32_t data_len) {
    rc_init(&ctx->rc, data, data_len);
    for (uint32_t i = 0; i < N_CTX; i++) ctx->prob[i] = (uint16_t)PROB_INIT;
    for (uint32_t i = 0; i < BA_FRAME_BYTES; i++) {
        ctx->framebuf[i] = 0;
        ctx->diff_buf[0][i] = 0;
        ctx->diff_buf[1][i] = 0;
    }
    ctx->cur_idx = 0;
    ctx->have_prev = false;
}

static inline void ba_decode_frame(ba_decoder_t *ctx, ba_draw_block_fn draw_cb, void *user) {
    uint8_t *cur_diff  = ctx->diff_buf[ctx->cur_idx];
    uint8_t *prev_diff = ctx->diff_buf[ctx->cur_idx ^ 1u];

    for (uint32_t i = 0; i < BA_FRAME_BYTES; i++) cur_diff[i] = 0;

    for (int16_t y = 0; y < (int16_t)BA_LOGICAL_H; y++) {
        for (int16_t x = 0; x < (int16_t)BA_LOGICAL_W; x++) {
            uint16_t pos = (uint16_t)(y * BA_LOGICAL_W + x);
            uint8_t c = compute_context(cur_diff, prev_diff, ctx->have_prev, x, y);
            uint8_t bit = rc_decode_bit(&ctx->rc, &ctx->prob[c]);

            if (bit) {
                ba__set(cur_diff, pos, true);
                ba__xor(ctx->framebuf, pos, true);
                if (draw_cb) {
                    bool now_on = ba__get_unchecked(ctx->framebuf, (uint16_t)x, (uint16_t)y);
                    draw_cb((uint16_t)x, (uint16_t)y, now_on, user);
                }
            }
        }
    }
    ctx->cur_idx ^= 1u;
    ctx->have_prev = true;
}

#endif /* BAD_APPLE_RC_H */
