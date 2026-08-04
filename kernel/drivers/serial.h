/* serial.h — COM1 debug output */
#pragma once
#include "../boot/types.h"

static inline void serial_init() {
    // 115200 baud, 8N1
    out8(0x3F8 + 3, 0x80); // DLAB
    out8(0x3F8 + 0, 0x01); // 115200 divisor low
    out8(0x3F8 + 1, 0x00); // 115200 divisor high
    out8(0x3F8 + 3, 0x03); // 8N1
}

static inline int serial_tx_ready() { return in8(0x3F8 + 5) & 0x20; }
static inline void serial_putc(char c) { while (!serial_tx_ready()) {} out8(0x3F8, c); }
static inline void serial_puts(const char *s) { while (*s) serial_putc(*s++); }
static inline void serial_hex(u32 v) {
    static const char h[] = "0123456789ABCDEF";
    serial_putc(h[(v >> 4) & 0xF]);
    serial_putc(h[v & 0xF]);
}
