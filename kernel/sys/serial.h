/* sys/serial.h — COM1 debug output */
#pragma once
#include "../drivers/io.h"

static inline void serial_putc(char c) {
    for(volatile int _t=0; _t<200000; _t++){ if(in8(0x3FD) & 0x20) break; }  // Tx ready w/ timeout
    out8(0x3F8, c);
}

/* 日志落盘钩子：默认空 = 只写串口；FS/NVMe 就绪后由 logdisk_init() 装上 */
typedef void (*LogSink)(const char *);
static LogSink g_log_sink = 0;
static inline void log_set_sink(LogSink f) { g_log_sink = f; }

static inline void out_str(const char *s) {
    /* 双写：盘（sink，可选）+ 串口 */
    if (g_log_sink) g_log_sink(s);
    while (*s) serial_putc(*s++);
}

/* 仅串口（心跳之类的高频日志走这条，不落盘——否则盘上大半是心跳） */
static inline void out_str_serial(const char *s) {
    while (*s) serial_putc(*s++);
}

static inline void out_file_str(const char *s) {
    out_str(s);  /* 别名：现在串口 + 盘都写 */
}
