/* kernel/idt.h — IDT + PIC + PIT */
#pragma once
#include "../boot/types.h"
#include "../drivers/io.h"

struct IdtEntry {
    u16 lo;
    u16 sel;
    u8 ist;
    u8 attr;
    u16 mid;
    u32 hi;
    u32 _rsv;
}
__attribute__((packed));
extern IdtEntry idt[256];

void pic_init() {
    out8(0x21,0xFF);
    out8(0xA1,0xFF);
    out8(0x20,0x11);
    out8(0x21,0x20);
    out8(0x21,4);
    out8(0x21,1);
    out8(0xA0,0x11);
    out8(0xA1,0x28);
    out8(0xA1,2);
    out8(0xA1,1);
    out8(0x21,0xF8);
    out8(0xA1,0xEF);
}
void pit_init() {
    out8(0x43,0x36);
    u16 d=11931;
    out8(0x40,d&0xFF);
    out8(0x40,d>>8); }
extern u64 KERN_BASE;  /* runtime base: link addr + KERN_BASE = runtime addr */

void set_gate(int n, void *h) {
    volatile u64 KB = KERN_BASE;  /* 强制读——clang -O2 内联会复用 rax 把 KERN_BASE 读成垃圾（gate20 实测 0x3be6c37f） */
    u64 a = KB + (u64)h;  /* h is LINK address — add runtime base! */
    idt[n].lo=a;
    idt[n].mid=a>>16;
    idt[n].hi=a>>32; 
    idt[n].sel=0x08;
    idt[n].attr=0x8E;
}
void lidt_idt() {
    u64 idt_addr;
    __asm__ volatile("lea %1, %0" : "=r"(idt_addr) : "m"(idt[0]));  // RIP-relative!
    struct { u16 lim; u64 base; } __attribute__((packed)) d={sizeof(idt)-1, idt_addr};
    __asm__ volatile("lidt %0"::"m"(d));
}
