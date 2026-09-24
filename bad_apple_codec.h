#ifndef BAD_APPLE_CODEC_H
#define BAD_APPLE_CODEC_H

#include <stdint.h>
#include <stdbool.h>

#define BA_LOGICAL_W 32u
#define BA_LOGICAL_H 16u
#define BA_BITS_PER_FRAME (BA_LOGICAL_W * BA_LOGICAL_H)
#define BA_FRAME_BYTES (BA_BITS_PER_FRAME / 8u)

typedef void (*ba_draw_block_fn)(uint16_t bx, uint16_t by, bool on, void *user);

typedef struct {
    const uint8_t *data;
    uint32_t data_len;
    uint32_t byte_pos;
    uint8_t pending_action;
    uint8_t pending_run;
    uint8_t framebuf[BA_FRAME_BYTES];
} ba_decoder_t;

static inline void ba_init(ba_decoder_t *ctx, const uint8_t *data, uint32_t data_len)
{
    ctx->data = data;
    ctx->data_len = data_len;
    ctx->byte_pos = 0;
    ctx->pending_action = 0;
    ctx->pending_run = 0;
    for (uint32_t i = 0; i < BA_FRAME_BYTES; i++)
        ctx->framebuf[i] = 0;
}

static inline bool ba_get_bit(const ba_decoder_t *ctx, uint16_t bx, uint16_t by)
{
    uint16_t pos = by * BA_LOGICAL_W + bx;
    return (ctx->framebuf[pos >> 3] >> (7 - (pos & 7))) & 1u;
}

static inline void ba_set_bit(ba_decoder_t *ctx, uint16_t pos, bool on)
{
    uint8_t mask = (uint8_t)(0x80u >> (pos & 7));
    if (on) ctx->framebuf[pos >> 3] |= mask;
    else    ctx->framebuf[pos >> 3] &= (uint8_t)~mask;
}

static inline bool ba_decode_frame(ba_decoder_t *ctx, ba_draw_block_fn draw_cb, void *user)
{
    uint16_t consumed = 0;
    while (consumed < BA_BITS_PER_FRAME) {
        if (ctx->pending_run == 0) {
            if (ctx->byte_pos >= ctx->data_len)
                return false;
            uint8_t token = ctx->data[ctx->byte_pos++];
            ctx->pending_action = (uint8_t)((token >> 6) & 0x2u);
            ctx->pending_run = (uint8_t)((token & 0x3Fu) + 1u);
        }
        uint16_t take = ctx->pending_run;
        if (take > (BA_BITS_PER_FRAME - consumed))
            take = (uint16_t)(BA_BITS_PER_FRAME - consumed);
        if (ctx->pending_action != 0) {
            for (uint16_t k = 0; k < take; k++) {
                uint16_t pos = (uint16_t)(consumed + k);
                bool was = ba_get_bit(ctx, (uint16_t)(pos % BA_LOGICAL_W),
                                            (uint16_t)(pos / BA_LOGICAL_W));
                ba_set_bit(ctx, pos, !was);
                if (draw_cb)
                    draw_cb((uint16_t)(pos % BA_LOGICAL_W), (uint16_t)(pos / BA_LOGICAL_W),
                            !was, user);
            }
        }
        consumed = (uint16_t)(consumed + take);
        ctx->pending_run = (uint8_t)(ctx->pending_run - take);
    }
    return true;
}

#endif /* BAD_APPLE_CODEC_H */
