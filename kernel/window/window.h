/* ============================================================
 * window.h — 图层系统（30dayOS/haribote 的 sheet.c 原版移植）
 *
 * 核心概念：
 *   SHEET（图层）= 一个窗口/光标：自己的像素缓冲(buf) + 位置(vx0,vy0)
 *                 + 大小(bxsize,bysize) + 透明色(col_inv)
 *   SHTCTL（图层管理器）= 管理所有图层的叠放顺序(sheets[]按高度排)
 *   sid = 图层在池里的编号（sht - sheets0 地址差）——标记"像素属于谁"
 *
 * 刷新两步走：
 *   sheet_refreshmap = 把图层可见像素写进 map（高层盖低层——"算账"）
 *   sheet_refreshsub = 按 map 把像素画到屏幕（"付钱"）
 *   sheet_slide（移动）= 旧位置 map 清除 + 新位置登记 + 两处重画
 *
 * u32 适配：原版的 4 字节快速路径(sid4)是 8 位色专属，u32 下语义错位，
 * 用等价的 1 字节逻辑代替。其余（结构/排序/updown/refresh/slide）
 * 逐行照抄原版 sheet.c。
 * ============================================================ */
#pragma once
#include "../boot/types.h"

#define MAX_SHEETS 16
#define COL_INV    0x00FF00FF   /* transparent (magenta) */
#define SHEET_USE  1

struct SHTCTL;

/* ── original struct SHEET (bootpack.h) ── */
struct SHEET {
    u32 *buf;                 /* unsigned char *buf in original */
    int bxsize, bysize;
    int vx0, vy0;
    u32 col_inv;              /* int col_inv in original */
    int height, flags;
    struct SHTCTL *ctl;
};

/* ── original struct SHTCTL (bootpack.h) ── */
struct SHTCTL {
    u32 *vram;                /* unsigned char *vram in original */
    u8 *map;
    int xsize, ysize, top;
    struct SHEET *sheets[MAX_SHEETS];
    struct SHEET sheets0[MAX_SHEETS];
};

/* 初始化图层管理器：登记 framebuffer/map/分辨率，所有图层池标记为未使用 */
extern struct SHEET *g_shadow_shts[8];   /* ★ 阴影窗口列表（半透明合成）——刷新联动（kernel.cpp 定义） */
extern int g_shadow_cnt;

static inline void shtctl_init(struct SHTCTL *ctl, u32 *vram, int xsize, int ysize, u8 *map) {
    ctl->vram = vram;
    ctl->map = map;
    ctl->xsize = xsize;
    ctl->ysize = ysize;
    ctl->top = -1;
    for (int i = 0; i < MAX_SHEETS; i++) {
        ctl->sheets0[i].flags = 0;
        ctl->sheets0[i].ctl = ctl;
    }
}

/* 从池里分配一个图层（sheets0[] 里第一个空闲的），返回指针；池满返回 0 */
static inline struct SHEET *sheet_alloc(struct SHTCTL *ctl) {
    for (int i = 0; i < MAX_SHEETS; i++) {
        if (ctl->sheets0[i].flags == 0) {
            struct SHEET *sht = &ctl->sheets0[i];
            sht->flags = SHEET_USE;
            sht->height = -1;
            return sht;
        }
    }
    return 0;
}

/* 给图层绑定像素缓冲 + 大小 + 透明色（col_inv：这个颜色的像素不算内容） */
static inline void sheet_setbuf(struct SHEET *sht, u32 *buf, int xsize, int ysize, u32 col_inv) {
    sht->buf = buf;
    sht->bxsize = xsize;
    sht->bysize = ysize;
    sht->col_inv = col_inv;
}

/* 刷新映射表：把 vx0..vy1 区域内、从高度 h0 往上的所有图层
   可见像素登记到 map（高层覆盖低层——"算账"）
   注意 vy/vx 越界检查（窗口拖出屏幕时的防线——之前没检查会写坏内存） */
static inline void sheet_refreshmap(struct SHTCTL *ctl, int vx0, int vy0, int vx1, int vy1, int h0) {
    int h, bx, by, vx, vy, bx0, by0, bx1, by1;
    u32 *buf;
    u8 sid, *map = ctl->map;
    struct SHEET *sht;
    if (vx0 < 0) vx0 = 0;
    if (vy0 < 0) vy0 = 0;
    if (vx1 > ctl->xsize) vx1 = ctl->xsize;
    if (vy1 > ctl->ysize) vy1 = ctl->ysize;
    for (h = h0; h <= ctl->top; h++) {
        sht = ctl->sheets[h];
        sid = (u8)(sht - ctl->sheets0);   /* address diff = layer id (ORIGINAL) */
        buf = sht->buf;
        bx0 = vx0 - sht->vx0;
        by0 = vy0 - sht->vy0;
        bx1 = vx1 - sht->vx0;
        by1 = vy1 - sht->vy0;
        if (bx0 < 0) bx0 = 0;
        if (by0 < 0) by0 = 0;
        if (bx1 > sht->bxsize) bx1 = sht->bxsize;
        if (by1 > sht->bysize) by1 = sht->bysize;
        if (sht->col_inv == (u32)-1) {
            /* no-transparency layer: map everything (original 4-byte path) */
            for (by = by0; by < by1; by++) {
                vy = sht->vy0 + by;
                if (vy >= 0 && vy < ctl->ysize) {
                    #pragma clang loop unroll(disable)
                    for (bx = bx0; bx < bx1; bx++) {
                        vx = sht->vx0 + bx;
                        if (vx >= 0 && vx < ctl->xsize) {
                            map[vy * ctl->xsize + vx] = sid;
                        }
                    }
                }
            }
        } else {
            /* transparency layer: skip col_inv pixels (ORIGINAL) */
            for (by = by0; by < by1; by++) {
                vy = sht->vy0 + by;
                if (vy >= 0 && vy < ctl->ysize) {
                    /* 关键坑：clang -O2 把循环展开成"步长2 + bx==bx1 退出"，
                       当 (bx1-bx0) 是奇数时 bx 永远不"相等"→ 死循环
                       （窗口拖出屏幕时踩过）。禁掉展开。 */
                    #pragma clang loop unroll(disable)
                    for (bx = bx0; bx < bx1; bx++) {
                        vx = sht->vx0 + bx;
                        if (vx >= 0 && vx < ctl->xsize) {
                            u32 pcol = buf[by * sht->bxsize + bx];
                            if (pcol != sht->col_inv && !(pcol & 0xFF000000)) {
                                /* ★ 半透明（阴影——高位 0xFF）不登记——下层可见（map=0） */
                                map[vy * ctl->xsize + vx] = sid;
                            }
                        }
                    }
                }
            }
        }
    }
}

/* 真正画像素：区域内、高度 h0..h1 的图层，凡是 map 标记"属于我"(==sid)
   的像素就画到 framebuffer（"付钱"）
   同样有 vy/vx 越界防线 */
static inline void sheet_refreshsub(struct SHTCTL *ctl, int vx0, int vy0, int vx1, int vy1, int h0, int h1) {
    int h, bx, by, vx, vy, bx0, by0, bx1, by1;
    u32 *buf, *vram = ctl->vram;
    u8 *map = ctl->map, sid;
    struct SHEET *sht;
    if (vx0 < 0) vx0 = 0;
    if (vy0 < 0) vy0 = 0;
    if (vx1 > ctl->xsize) vx1 = ctl->xsize;
    if (vy1 > ctl->ysize) vy1 = ctl->ysize;
    for (h = h0; h <= h1; h++) {
        sht = ctl->sheets[h];
        buf = sht->buf;
        sid = (u8)(sht - ctl->sheets0);
        bx0 = vx0 - sht->vx0;
        by0 = vy0 - sht->vy0;
        bx1 = vx1 - sht->vx0;
        by1 = vy1 - sht->vy0;
        if (bx0 < 0) bx0 = 0;
        if (by0 < 0) by0 = 0;
        if (bx1 > sht->bxsize) bx1 = sht->bxsize;
        if (by1 > sht->bysize) by1 = sht->bysize;
        for (by = by0; by < by1; by++) {
            vy = sht->vy0 + by;
            if (vy >= 0 && vy < ctl->ysize) {
                #pragma clang loop unroll(disable)
                for (bx = bx0; bx < bx1; bx++) {
                    vx = sht->vx0 + bx;
                    if (vx >= 0 && vx < ctl->xsize) {
                        if (map[vy * ctl->xsize + vx] == sid
                            || (h == sht->height && (buf[by * sht->bxsize + bx] & 0xFF000000))) {
                            /* ★ 半透明（阴影）像素：当前层自己——无论单层/多层刷新都画（合成）
                               （map 是 0——下层可见——但本层阴影必须画——否则光标移动会"擦掉"阴影） */
                            u32 pix = buf[by * sht->bxsize + bx];
                            if (pix & 0xFF000000) {
                                /* ★ 阴影合成：像素 = 下层 − depth×step
                                   （越靠窗 depth 越大——每层减 step——变暗渐变） */
                                u32 depth = pix & 0xFF;   /* 层数（draw_win_shadow 编码） */
                                u32 dst = vram[vy * ctl->xsize + vx];
                                u32 add = depth;   /* 每层 −1（step=1——浅阴影） */
                                u32 r = (dst >> 16) & 0xFF, g = (dst >> 8) & 0xFF, b = dst & 0xFF;
                                /* 减：阴影变暗（if/else clamp——三元会触发 clang -O2 优化陷阱 E06） */
                                if (r > add) r -= add; else r = 0;
                                if (g > add) g -= add; else g = 0;
                                if (b > add) b -= add; else b = 0;
                                vram[vy * ctl->xsize + vx] = (r << 16) | (g << 8) | b;
                            } else {
                                vram[vy * ctl->xsize + vx] = pix;
                            }
                        }
                    }
                }
            }
        }
    }
}

/* 改变图层高度（叠放顺序）：-1=隐藏，0=最底，top=最顶
   内部把 sheets[] 数组重排（保持按高度有序）+ 刷新受影响区域
   点击窗口置顶/关闭隐藏都走这里 */
static inline void sheet_updown(struct SHEET *sht, int height) {
    struct SHTCTL *ctl = sht->ctl;
    int h, old = sht->height;
    if (height > ctl->top + 1) height = ctl->top + 1;
    if (height < -1) height = -1;
    sht->height = height;
    if (old > height) {
        if (height >= 0) {
            for (h = old; h > height; h--) {
                ctl->sheets[h] = ctl->sheets[h - 1];
                ctl->sheets[h]->height = h;
            }
            ctl->sheets[height] = sht;
            sheet_refreshmap(ctl, sht->vx0, sht->vy0, sht->vx0 + sht->bxsize, sht->vy0 + sht->bysize, height + 1);
            sheet_refreshsub(ctl, sht->vx0, sht->vy0, sht->vx0 + sht->bxsize, sht->vy0 + sht->bysize, height + 1, old);
        } else {
            if (ctl->top > old) {
                for (h = old; h < ctl->top; h++) {
                    ctl->sheets[h] = ctl->sheets[h + 1];
                    ctl->sheets[h]->height = h;
                }
            }
            ctl->top--;
            sheet_refreshmap(ctl, sht->vx0, sht->vy0, sht->vx0 + sht->bxsize, sht->vy0 + sht->bysize, 0);
            sheet_refreshsub(ctl, sht->vx0, sht->vy0, sht->vx0 + sht->bxsize, sht->vy0 + sht->bysize, 0, old - 1);
        }
    } else if (old < height) {
        if (old >= 0) {
            for (h = old; h < height; h++) {
                ctl->sheets[h] = ctl->sheets[h + 1];
                ctl->sheets[h]->height = h;
            }
            ctl->sheets[height] = sht;
        } else {
            for (h = ctl->top; h >= height; h--) {
                ctl->sheets[h + 1] = ctl->sheets[h];
                ctl->sheets[h + 1]->height = h + 1;
            }
            ctl->sheets[height] = sht;
            ctl->top++;
        }
        sheet_refreshmap(ctl, sht->vx0, sht->vy0, sht->vx0 + sht->bxsize, sht->vy0 + sht->bysize, height);
        sheet_refreshsub(ctl, sht->vx0, sht->vy0, sht->vx0 + sht->bxsize, sht->vy0 + sht->bysize, height, height);
    }
}

/* 刷新窗口内部的一个区域（坐标是窗口内相对坐标）——
   比如登录窗口输入框变了就刷新输入框那一块，不用全屏重画 */
static inline void sheet_refresh(struct SHEET *sht, int bx0, int by0, int bx1, int by1) {
    if (sht->height >= 0) {
        sheet_refreshsub(sht->ctl, sht->vx0 + bx0, sht->vy0 + by0,
                         sht->vx0 + bx1, sht->vy0 + by1, sht->height, sht->height);
        /* ★ 阴影联动：任意层刷新后——若与阴影层区域重叠——重画阴影层（合成读新下层） */
        for (int si = 0; si < g_shadow_cnt; si++) {
            struct SHEET *gs = g_shadow_shts[si];
            if (gs && gs != sht && gs->height >= 0) {
                int sx0 = gs->vx0, sy0 = gs->vy0;
                int sx1 = sx0 + gs->bxsize, sy1 = sy0 + gs->bysize;
                int rx0 = sht->vx0 + bx0, ry0 = sht->vy0 + by0;
                int rx1 = sht->vx0 + bx1, ry1 = sht->vy0 + by1;
                if (rx0 < sx1 && rx1 > sx0 && ry0 < sy1 && ry1 > sy0) {
                    sheet_refreshsub(sht->ctl, sx0, sy0, sx1, sy1, gs->height, gs->height);
                }
            }
        }
    }
}

/* 移动图层到新位置（窗口拖拽/光标移动都走这）：
   1. 旧位置 map 清除（从高度 0 重算——该处被谁挡住就归谁）
   2. 新位置 map 登记（本图层及以上的）
   3. 旧位置重画（只画比本图层低的——擦掉旧影子）
   4. 新位置重画（画本图层自己）
   只刷两个小区域——快，不全屏重画 */
static inline void sheet_slide(struct SHEET *sht, int vx0, int vy0) {
    struct SHTCTL *ctl = sht->ctl;
    int old_vx0 = sht->vx0, old_vy0 = sht->vy0;
    sht->vx0 = vx0;
    sht->vy0 = vy0;
    if (sht->height >= 0) {
        sheet_refreshmap(ctl, old_vx0, old_vy0, old_vx0 + sht->bxsize, old_vy0 + sht->bysize, 0);
        sheet_refreshmap(ctl, vx0, vy0, vx0 + sht->bxsize, vy0 + sht->bysize, sht->height);
        sheet_refreshsub(ctl, old_vx0, old_vy0, old_vx0 + sht->bxsize, old_vy0 + sht->bysize, 0, sht->height - 1);
        sheet_refreshsub(ctl, vx0, vy0, vx0 + sht->bxsize, vy0 + sht->bysize, sht->height, sht->height);
    }
}

/* 释放图层（先隐藏再从池里归还）——点窗口关闭按钮时调用 */
static inline void sheet_free(struct SHEET *sht) {
    if (sht->height >= 0) sheet_updown(sht, -1);
    sht->flags = 0;
}

/* 全屏刷新（启动时/登录后调用）：
   背景用连续写直接画（快——逐像素 map 检查写 48 万像素太慢）；
   背景图层在这跳过（它只在 sheet_slide 的小区域刷新里用）。
   其余图层正常 refreshmap + refreshsub 画上 */
static inline void shtctl_refresh_all(struct SHTCTL *ctl) {
    /* ★ 用 volatile 强制每次从 ctl->vram 加载基址——编译器内联后曾把背景写循环的
       基址(rax)复用成调用点垃圾值(0xFF490008)→#PF。volatile 阻止该复用 */
    volatile u32 *v = ctl->vram;
    for (u32 y = 0; y < (u32)ctl->ysize; y++) {
        u32 bg = (y >= (u32)ctl->ysize - 40) ? 0 : 0x00008080;
        for (u32 x = 0; x < (u32)ctl->xsize; x++) v[y * ctl->xsize + x] = bg;
    }
    sheet_refreshmap(ctl, 0, 0, ctl->xsize, ctl->ysize, 0);
    sheet_refreshsub(ctl, 0, 0, ctl->xsize, ctl->ysize, 1, ctl->top);
}
