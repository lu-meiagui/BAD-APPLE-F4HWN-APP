#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "../app_api.h"
#include "bad_apple_rc.h"
#include "bad_apple_clip.h"

#define BLOCK 4
#define FRAME_MS 333

static const app_api_t *A;

void *memset(void *d, int c, size_t n) {
    uint8_t *p = d; while (n--) *p++ = (uint8_t)c; return d;
}

static ba_decoder_t dec;

static void draw_block(uint16_t bx, uint16_t by, bool on, void *user) {
    (void)user;
    int16_t x1 = (int16_t)(bx * BLOCK);
    int16_t y1 = (int16_t)(by * BLOCK);
    A->draw_rect(A->fb, x1, y1, (int16_t)(x1 + BLOCK - 1), (int16_t)(y1 + BLOCK - 1), !on);
}

__attribute__((section(".text.entry"), used))
void app_main(const app_api_t *api) {
    A = api;

    A->display_clear();
    A->status_clear();
    A->backlight_on();

    ba_init(&dec, bad_apple_data, BAD_APPLE_DATA_LEN);

    uint16_t frame = 0;
    bool running = true;

    while (running) {
        if (A->get_key() == APP_KEY_EXIT)
            break;

        ba_decode_frame(&dec, draw_block, NULL);
        A->blit_full();
        A->delay_ms(FRAME_MS);

        frame++;
        if (frame >= BA_SRC_NUM_FRAMES) {
            frame = 0;
            ba_init(&dec, bad_apple_data, BAD_APPLE_DATA_LEN);
            A->display_clear();
        }
    }
}
