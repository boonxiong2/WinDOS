/* sys/serial.h — COM1 debug output */
#pragma once
#include "../drivers/io.h"

static inline void serial_putc(char c) {
    for(volatile int _t=0; _t<200000; _t++){ if(in8(0x3FD) & 0x20) break; }  // Tx ready w/ timeout
    out8(0x3F8, c);
}

static inline void out_str(const char *s) {
    while (*s) serial_putc(*s++);
}

static inline void out_file_str(const char *s) {
    out_str(s);  // alias — to file when FS ready
}
