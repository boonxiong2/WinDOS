/* sys/draw.h — 带裁剪的 2D 基元（fill / blit / text）
 *
 * 目的：把"越界写 = 踩别的全局变量（症状是崩在完全无关的地方）"变成"顶多画不出来"。
 * 它们**不替代**图层系统——sheet 自己的 refreshsub/refreshmap 早就有屏幕裁剪了；
 * 这层是给"直接往缓冲上画"的地方兜底（窗口缓冲、back_buf、任务栏、死屏等）。
 *
 * 调用约定：
 *   fb      : 目标缓冲（u32*）
 *   stride  : 每行像素数（= 缓冲宽度）
 *   fw, fh  : 目标缓冲的**逻辑尺寸**（用于裁剪；不是字节数）
 *   key     : blit 的透明色；(u32)-1 = 不透明整块拷
 *
 * 注意：不要在循环里用 continue——本项目被 clang -O2 + continue 坑过（跳循环尾不递增 → 死循环）。
 */
#pragma once
#include "../boot/types.h"
#include "fill.h"                 /* 旧的 fill_rect 保留不动 */
#include "../drivers/font_data.h"  /* font[256][16]（put_char_c 用） */

/* 求「要画的矩形」和「缓冲」的交
   返回：0 = 完全在界外（什么都不用画）；1 = 有效，且 (x,y,w,h) 已就地修正为可见部分 */
static inline int clip_rect(int *x, int *y, int *w, int *h, int fw, int fh) {
    long x0 = *x, y0 = *y, x1 = (long)*x + *w, y1 = (long)*y + *h;   /* long 防加法溢出 */
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > fw) x1 = fw;
    if (y1 > fh) y1 = fh;
    if (x1 <= x0 || y1 <= y0) return 0;
    *x = (int)x0; *y = (int)y0; *w = (int)(x1 - x0); *h = (int)(y1 - y0);
    return 1;
}

/* 带裁剪的填充 */
static inline void fill_rect_c(u32 *fb, u32 stride, int fw, int fh,
                               int x, int y, int w, int h, u32 color) {
    if (!clip_rect(&x, &y, &w, &h, fw, fh)) return;
    for (int dy = 0; dy < h; dy++) {
        u32 *row = fb + (u32)(y + dy) * stride + (u32)x;
        for (int dx = 0; dx < w; dx++) row[dx] = color;
    }
}

/* 带裁剪的块拷贝（窗口合成 / 贴图 / 字形缓存）
   key != (u32)-1 时跳过等于 key 的像素（透明） */
static inline void blit_c(u32 *dst, u32 dstride, int dw, int dh,
                          const u32 *src, u32 sstride,
                          int x, int y, int w, int h, u32 key) {
    int sx = 0, sy = 0;
    if (x < 0) { sx = -x; w += x; x = 0; }        /* 左边出界：源跟着往右挪 */
    if (y < 0) { sy = -y; h += y; y = 0; }        /* 上边出界：源跟着往下挪 */
    if (x + w > dw) w = dw - x;                   /* 右边出界：截断宽度 */
    if (y + h > dh) h = dh - y;                   /* 下边出界：截断高度 */
    if (w <= 0 || h <= 0) return;
    for (int i = 0; i < h; i++) {
        const u32 *s = src + (u32)(sy + i) * sstride + (u32)sx;
        u32 *d = dst + (u32)(y + i) * dstride + (u32)x;
        if (key == (u32)-1) {
            for (int j = 0; j < w; j++) d[j] = s[j];
        } else {
            for (int j = 0; j < w; j++) if (s[j] != key) d[j] = s[j];
        }
    }
}

/* 带裁剪的字符输出（8x16 点阵；越界部分自动丢掉） */
static inline void put_char_c(u32 *fb, u32 stride, int fw, int fh,
                              int x, int y, char c, u32 color) {
    const u8 *g = font[(u8)c];
    for (int dy = 0; dy < 16; dy++) {
        int py = y + dy;
        u8 bits = g[dy];
        if (py >= 0 && py < fh && bits) {
            u32 *row = fb + (u32)py * stride;
            for (int dx = 0; dx < 8; dx++) {
                int px = x + dx;
                if (px >= 0 && px < fw && (bits & (0x80 >> dx))) row[px] = color;
            }
        }
    }
}
static inline void put_str_c(u32 *fb, u32 stride, int fw, int fh,
                             int x, int y, const char *s, u32 color) {
    while (*s) { put_char_c(fb, stride, fw, fh, x, y, *s++, color); x += 8; }
}

/* 像素级 alpha 混合（0..255）——阴影/淡入淡出用；除法换成 (x*257+32768)>>16 近似 */
static inline u32 blend_alpha(u32 d, u32 s, u32 a) {
    u32 dr = (d >> 16) & 0xFF, dg = (d >> 8) & 0xFF, db = d & 0xFF;
    u32 sr = (s >> 16) & 0xFF, sg = (s >> 8) & 0xFF, sb = s & 0xFF;
    u32 r = (sr * a + dr * (255 - a) + 128) * 257 >> 16;
    u32 g = (sg * a + dg * (255 - a) + 128) * 257 >> 16;
    u32 b = (sb * a + db * (255 - a) + 128) * 257 >> 16;
    return (r << 16) | (g << 8) | b;
}

/* ── 哨兵自检：证明裁剪真的挡住了越界写 ──
   越界写不一定立刻崩，但一定会改掉哨兵；所以这比"看它崩不崩"可靠。
   返回 0 = 通过。（由 kernel.cpp 打日志，避免这里再依赖日志头） */
static inline int draw_selftest(void) {
    enum { N = 64, PAD = 64 };
    static u32 sb[N * N + 2 * PAD];
    static u32 src[2 * N * N];
    for (int i = 0; i < 2 * N * N; i++) src[i] = 0x00112233u;
    /* 哨兵必须在缓冲【之外】：buf = sb+PAD 覆盖 sb[PAD .. PAD+N*N-1]
       所以后哨兵从 sb[PAD+N*N] 起（曾写成 PAD+N*N-1-i → 与缓冲尾重叠 → 自己误报） */
    for (int i = 0; i < PAD; i++) {
        sb[i] = 0xDEADBEEFu;
        sb[PAD + N * N + i] = 0xDEADBEEFu;
    }
    u32 *buf = sb + PAD;
    /* 故意全画到界外（四边都试） */
    fill_rect_c(buf, N, N, N, -50, -50, 400, 400, 0x00FF0000u);
    fill_rect_c(buf, N, N, N, N - 4, N - 4, 400, 400, 0x0000FF00u);
    blit_c(buf, N, N, N, src, N, -20, N - 10, 100, 100, (u32)-1);
    blit_c(buf, N, N, N, src, N, N - 6, -30, 100, 100, (u32)-1);
    put_str_c(buf, N, N, N, -20, N - 8, "AAAA", 0x00FFFFFFu);
    put_str_c(buf, N, N, N, N - 4, N - 4, "ZZZZ", 0x00FFFFFFu);
    fill_rect_c(buf, N, N, N, 0, 0, N, N, 0x00000000u);   /* 整块在界内：应当画到 */
    for (int i = 0; i < PAD; i++) {
        if (sb[i] != 0xDEADBEEFu || sb[PAD + N * N + i] != 0xDEADBEEFu) return 1;   /* 哨兵被改 = 越界了 */
    }
    if (buf[0] != 0x00000000u || buf[N * N - 1] != 0x00000000u) return 2;               /* 界内应当画上 */
    return 0;
}
