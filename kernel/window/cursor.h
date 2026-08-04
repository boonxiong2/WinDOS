/* cursor.h — software cursor with classic WinDOS arrow */
#pragma once
#include "../boot/types.h"

struct Cursor { int x, y; };

// 18×16 arrow cursor: 1=black edge, 2=white fill, 0=transparent
static const u8 cursor_shape[16][18] = {
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,1,0,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,1,0,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,2,2,1,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,2,2,2,1,0,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,2,2,2,2,1,0,0,0,0,0,0,0,0},
    {1,2,2,2,2,2,1,1,1,1,1,0,0,0,0,0,0,0},
    {1,2,2,1,2,2,1,0,0,0,0,0,0,0,0,0,0,0},
    {1,2,2,1,1,2,2,1,0,0,0,0,0,0,0,0,0,0},
    {1,1,0,0,1,2,2,1,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,1,2,2,1,0,0,0,0,0,0,0,0,0,0},
    {0,0,0,0,0,1,2,2,1,0,0,0,0,0,0,0,0,0},
};

static inline void cursor_draw(u32 *fb, u32 stride, Cursor *c) {
    for (u32 dy = 0; dy < 16; dy++) {
        for (u32 dx = 0; dx < 18; dx++) {
            u8 p = cursor_shape[dy][dx];
            if (p == 0) continue;
            u32 color = (p == 1) ? 0x00000000 : 0x00FFFFFF;  // black edge, white fill
            fb[(c->y + dy) * stride + c->x + dx] = color;
        }
    }
}
/*
static inline void cursor_erase(u32 *fb, u32 stride, Cursor *c, u32 hr, u32 vr) {
    for (u32 dy = 0; dy < 16; dy++) {
        u32 bg = ((u32)c->y + dy >= vr - 40) ? 0 : 0x00008080;
        for (u32 dx = 0; dx < 18; dx++) {
            int px = c->x + (int)dx;
            if (px < 0 || (u32)px >= hr) continue;
            fb[(c->y + dy) * stride + px] = bg;
        }
    }
}
*/