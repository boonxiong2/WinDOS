/* desk/page.h — 页分配器（位图）
   池有两种来源：
     1) page_init(base, bytes) —— 用引导器经 UEFI 分配的堆（推荐，动态）
     2) 没调用 page_init 时回落到本文件内的静态小池（8 页，仅够早期自检）
   页要求：4KB 对齐 + 物理连续。identity map 下虚拟地址 == 物理地址，可直接交硬件。 */
#pragma once
#include "../boot/types.h"

#define PAGE_SIZE   4096
#define PAGE_MAX    16384                      /* 位图容量：16384 页 = 64MB */
#define PAGE_BMP_SZ (PAGE_MAX / 8)

/* 静态回落池（很小：只在引导器没给堆时用） */
static u8  page_pool_static[8 * PAGE_SIZE] __attribute__((aligned(4096)));
static u8 *g_pool = page_pool_static;
static u32 g_pages = 8;
static u8  page_bmp[PAGE_BMP_SZ] __attribute__((aligned(16)));
static u32 page_free_cnt = 8;

/* 用外部堆初始化（base 必须 4KB 对齐；bytes 向下取整到页） */
static int page_init(u64 base, u64 bytes) {
    if (!base || (base & 0xFFF) || bytes < PAGE_SIZE) return -1;
    u32 pages = (u32)(bytes / PAGE_SIZE);
    if (pages > PAGE_MAX) pages = PAGE_MAX;
    g_pool  = (u8*)base;
    g_pages = pages;
    for (u32 i = 0; i < PAGE_BMP_SZ; i++) page_bmp[i] = 0;
    page_free_cnt = pages;
    return 0;
}

static inline int page_is_used(u32 i) { return (page_bmp[i >> 3] >> (i & 7)) & 1; }
static inline void page_set(u32 i)    { page_bmp[i >> 3] |= (u8)(1 << (i & 7)); }
static inline void page_clr(u32 i)    { page_bmp[i >> 3] &= (u8)~(1 << (i & 7)); }

/* 分配 n 个连续页；返回 4KB 对齐地址，失败返回 0 */
static void *page_alloc(u32 n) {
    if (n == 0 || n > page_free_cnt) return 0;
    u32 run = 0;
    for (u32 i = 0; i < g_pages; i++) {
        if (page_is_used(i)) { run = 0; continue; }
        if (++run == n) {
            u32 base = i + 1 - n;
            for (u32 k = 0; k < n; k++) page_set(base + k);
            page_free_cnt -= n;
            return &g_pool[(u64)base * PAGE_SIZE];
        }
    }
    return 0;
}

/* 释放（调用方必须知道当初分配了几页） */
static void page_free(void *ptr, u32 n) {
    if (!ptr || !g_pool) return;
    u64 off = (u64)((u8*)ptr - g_pool);
    if (off % PAGE_SIZE) return;
    u32 base = (u32)(off / PAGE_SIZE);
    if (base + n > g_pages) return;
    for (u32 k = 0; k < n; k++)
        if (page_is_used(base + k)) { page_clr(base + k); page_free_cnt++; }
}

static inline u32 page_free_pages(void) { return page_free_cnt; }
static inline u32 page_total_pages(void) { return g_pages; }

/* 自检：分配/对齐/连续/写满/释放回收——全对返回 0，否则返回失败步骤号 */
static int page_selftest(void) {
    u32 before = page_free_pages();
    if (before < 40) return 1;                      /* 池太小，跳过（静态回落池） */
    u8 *a = (u8*)page_alloc(1);
    if (!a) return 2;
    if (((u64)a & 0xFFF) != 0) return 3;
    u8 *b = (u8*)page_alloc(32);
    if (!b) return 4;
    if (((u64)b & 0xFFF) != 0) return 5;
    if (b != a + PAGE_SIZE) return 6;
    for (u32 i = 0; i < PAGE_SIZE; i++) b[31 * PAGE_SIZE + i] = (u8)i;
    if (b[31 * PAGE_SIZE + 100] != (u8)100) return 7;
    page_free(a, 1);
    page_free(b, 32);
    if (page_free_pages() != before) return 8;
    u8 *c = (u8*)page_alloc(1);
    if (c != a) return 9;
    page_free(c, 1);
    if (page_free_pages() != before) return 10;
    return 0;
}
