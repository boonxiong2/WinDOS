/* sys/logdisk.h — 日志落盘（NVMe 裸扇区，一个简易 ring）
   思路：给 serial 的日志系统装一个 sink —— 所有 out_str() 除了走串口，
        同时往 4KB 缓冲里攒；攒满一页就写 NVMe 的连续 LBA。
   位置：LBA 2048 起（= 1MB 偏移），最多 2MB（1024 扇区）；写满就停止（不覆盖开头，
        这样第一段启动日志永远在盘上，方便取证）。
   注意：不依赖文件系统 —— 裸扇区，天然避开 exFAT 的"同名覆盖"问题。
   心跳（SYSTEM ALIVE）等高频日志请用 out_str_serial()，不要走这里。 */
#pragma once
#include "../boot/types.h"
#include "../fs/nvme.h"
#include "serial.h"

#define LOGDISK_LBA0       2048u                     /* 1MB 偏移 */
#define LOGDISK_MAX_SECT   1024u                     /* 最多写 512KB */
#define LOGDISK_BUF_SZ     4096u                     /* 一页攒满写一次 */

static u8  g_logbuf[LOGDISK_BUF_SZ] __attribute__((aligned(4096)));
static u32 g_loglen = 0;
static u32 g_logsect = 0;                             /* 已写扇区数 */
static int g_logdisk_ok = 0;

/* 把缓冲里攒的内容写盘（不足一页也写，按 512B 取整） */
static void logdisk_flush(void) {
    if (!g_logdisk_ok || g_loglen == 0) return;
    u32 nlb = (g_loglen + 511u) / 512u;
    if (g_logsect + nlb > LOGDISK_MAX_SECT) { g_loglen = 0; return; }   /* 区满：丢弃新日志 */
    if (nvme_rw(0x01, 1, (u64)(LOGDISK_LBA0 + g_logsect), nlb, g_logbuf) == 0) {
        g_logsect += nlb;
        g_loglen = 0;
    } else {
        g_loglen = 0;                                  /* 写失败也别卡住：丢掉这段 */
    }
}

/* sink 本体：只往缓冲里塞，满了才落盘 */
static void logdisk_sink(const char *s) {
    if (!g_logdisk_ok) return;
    while (*s) {
        if (g_loglen >= LOGDISK_BUF_SZ) logdisk_flush();
        if (g_loglen >= LOGDISK_BUF_SZ) return;
        g_logbuf[g_loglen++] = (u8)*s++;
    }
}

/* NVMe 就绪后调用：清缓冲 + 装 sink
   返回 0 = 已启用；-1 = NVMe 没就绪（保持只写串口） */
static int logdisk_init(void) {
    if (!g_nvme.ready) return -1;
    g_loglen = 0;
    g_logsect = 0;
    g_logdisk_ok = 1;
    log_set_sink(logdisk_sink);
    return 0;
}

static inline u32 logdisk_sectors_written(void) { return g_logsect; }
