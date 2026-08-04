/* sys/fill.h — fill screen/rect */
#pragma once
#include "../boot/types.h"

static inline void fill_rect(u32 *fb, u32 stride, int x, int y, int w, int h, u32 color) {
    for (int dy = 0; dy < h; dy++)
        for (int dx = 0; dx < w; dx++)
            fb[(y + dy) * stride + x + dx] = color;
}

static inline void fill_screen(u32 *fb, u32 stride, u32 w, u32 h, u32 color) {
    fill_rect(fb, stride, 0, 0, w, h, color);
}
