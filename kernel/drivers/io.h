/* io.h — port I/O */
#pragma once
#include "types.h"
static inline u8 in8(u16 p) { u8 v; __asm__("inb %1,%0":"=a"(v):"dN"(p)); return v; }
static inline void out8(u16 p, u8 v) { __asm__("outb %0,%1"::"a"(v),"dN"(p)); }
static inline void io_hlt() { __asm__ volatile("hlt"); }
static inline void io_cli() { __asm__ volatile("cli"); }
static inline void io_sti() { __asm__ volatile("sti"); }
static inline void io_stihlt() {
    __asm__ volatile("sti");
    __asm__ volatile("hlt");
}
