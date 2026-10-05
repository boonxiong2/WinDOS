/* desk/page.h — 页分配器（位图，静态池）
   用途：页表页 / DMA 缓冲 / NVMe 队列——要求 4KB 对齐 + 物理连续
   注意：identity map 下虚拟地址 == 物理地址，可以直接交给硬件
   池大小：PAGE_COUNT 页（改这个数就改池大小——注意 .bss 会跟着涨） */
#pragma once
#include "../boot/types.h"

#define PAGE_SIZE   4096
#define PAGE_COUNT  256                        /* 256 页 = 1MB 池 */
#define PAGE_BMP_SZ (PAGE_COUNT / 8)           /* 256 页 -> 32 字节位图 */

static u8 page_pool[PAGE_COUNT * PAGE_SIZE] __attribute__((aligned(4096)));
static u8 page_bmp[PAGE_BMP_SZ] __attribute__((aligned(16)));
static u32 page_free_cnt = PAGE_COUNT;

/* 位操作：bit=1 表示已用 */
static inline int page_is_used(u32 i) { return (page_bmp[i >> 3] >> (i & 7)) & 1; }
static inline void page_set(u32 i)    { page_bmp[i >> 3] |= (u8)(1 << (i & 7)); }
static inline void page_clr(u32 i)    { page_bmp[i >> 3] &= (u8)~(1 << (i & 7)); }

/* 分配 n 个连续页（n>=1）；返回 4KB 对齐地址，失败返回 0 */
static void *page_alloc(u32 n) {
    if (n == 0 || n > page_free_cnt) return 0;
    u32 run = 0;
    for (u32 i = 0; i < PAGE_COUNT; i++) {
        if (page_is_used(i)) { run = 0; continue; }
        if (++run == n) {
            u32 base = i + 1 - n;
            for (u32 k = 0; k < n; k++) page_set(base + k);
            page_free_cnt -= n;
            return &page_pool[base * PAGE_SIZE];
        }
    }
    return 0;
}

/* 释放（调用方必须知道当初分配了几页） */
static void page_free(void *ptr, u32 n) {
    if (!ptr) return;
    u64 off = (u64)((u8*)ptr - page_pool);
    if (off % PAGE_SIZE) return;                     /* 不是池内页起址 */
    u32 base = (u32)(off / PAGE_SIZE);
    if (base + n > PAGE_COUNT) return;
    for (u32 k = 0; k < n; k++) {
        if (page_is_used(base + k)) { page_clr(base + k); page_free_cnt++; }
    }
}

static inline u32 page_free_pages(void) { return page_free_cnt; }

/* 自检：分配/对齐/连续性/释放回收——全对返回 0，否则返回失败步骤号 */
static int page_selftest(void) {
    u32 before = page_free_pages();
    /* 1) 单页分配 + 4KB 对齐 */
    u8 *a = (u8*)page_alloc(1);
    if (!a) return 1;
    if (((u64)a & 0xFFF) != 0) return 2;
    /* 2) 多页连续分配 + 对齐 + 地址连续 */
    u8 *b = (u8*)page_alloc(32);
    if (!b) return 3;
    if (((u64)b & 0xFFF) != 0) return 4;
    if (b != a + PAGE_SIZE) return 5;                /* a 之后紧邻 */
    /* 3) 写满最后一页，验证池没有越界（写 4KB 不炸） */
    for (u32 i = 0; i < PAGE_SIZE; i++) b[31 * PAGE_SIZE + i] = (u8)i;
    if (b[31 * PAGE_SIZE + 100] != (u8)100) return 6;
    /* 4) 释放回收 */
    page_free(a, 1);
    page_free(b, 32);
    if (page_free_pages() != before) return 7;
    /* 5) 再分配应该拿到同样的地址（位图回收正确） */
    u8 *c = (u8*)page_alloc(1);
    if (c != a) return 8;
    page_free(c, 1);
    if (page_free_pages() != before) return 9;
    return 0;
}
