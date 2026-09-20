/* io.h — port I/O */
#pragma once
#include "types.h"
/* 端口 I/O 必须 volatile + "memory"：
   in8 带输出操作数，非 volatile 时 clang -O2 会把它当纯函数——CSE/提升出轮询循环，
   表现为"轮询状态位永远不变"（正是"IDE PIO DRQ 永不置位"的真凶）、多次读同一端口返回旧值。
   out8 无输出操作数（GCC 规则隐含 volatile）才侥幸活下来。 */
static inline u8 in8(u16 p) { u8 v; __asm__ volatile("inb %1,%0":"=a"(v):"dN"(p):"memory"); return v; }
static inline void out8(u16 p, u8 v) { __asm__ volatile("outb %0,%1"::"a"(v),"dN"(p):"memory"); }
static inline void io_hlt() { __asm__ volatile("hlt"); }
static inline void io_cli() { __asm__ volatile("cli"); }
static inline void io_sti() { __asm__ volatile("sti"); }
static inline void io_stihlt() {
    __asm__ volatile("sti");
    __asm__ volatile("hlt");
}
