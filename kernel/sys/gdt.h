/* sys/gdt.h — 64-bit GDT + TSS (Ring 1 user mode!) */
#pragma once
#include "../boot/types.h"

/* GDT layout matches SYSRET synthetic segments (NanOS convention):
   STAR user-CS base 0x18 → sysret CS=0x2B (gdt[5], DPL3 code64),
   SS=0x23 (gdt[4], DPL3 data). IRQ-return iretq also loads these. */
__attribute__((aligned(8))) static u64 gdt[12] = {
    0x0000000000000000,          // 0: null
    0x00AF9A000000FFFF,          // 1: kernel code  (0x08, DPL0, L=1)
    0x00CF92000000FFFF,          // 2: kernel data  (0x10, DPL0)
    0x00CFFA000000FFFF,          // 3: user code32 placeholder (0x18, DPL3; STAR base, never loaded)
    0x00CFF2000000FFFF,            // 4: user data Ring3 (0x20, DPL3) → sysret SS=0x23
    0x00AFFA000000FFFF,          // 5: user code64 Ring3 (0x28, DPL3, L=1) → sysret CS=0x2B
    0x0000000000000000,          // 6: TSS low  (0x30) — filled in gdt_init
    0x0000000000000000,          // 7: TSS high
    0x00AFBA000000FFFF,          // 8: user code Ring1 (0x40, DPL1) — legacy, unused with sysret
    0x00CFB2000000FFFF,            // 9: user data Ring1 (0x48, DPL1)
    0x0000000000000000,          // 10: spare
    0x0000000000000000           // 11: spare
};

/* x86-64 TSS: only RSP0 needed for interrupt entry from Ring 1 */
__attribute__((aligned(16))) static u8 tss[104];
static u64 tss_rsp0;

static void gdt_init() {
    /* TSS descriptor: 64-bit TSS type 0x89, base = &tss (RIP-relative!) */
    u64 tss_addr;
    __asm__ volatile("lea %1, %0" : "=r"(tss_addr) : "m"(tss[0]));
    u64 base = tss_addr, lim = 103;
    gdt[6] = (lim & 0xFFFF)            /* TSS now at gdt[6] (0x30) — sysret layout */
           | ((base & 0xFFFFFF) << 16)
           | (0x89ULL << 40)
           | (((base >> 24) & 0xFF) << 56);
    gdt[7] = (base >> 32) | (0ULL << 32);   // base 32-63

    u64 gdt_addr;
    __asm__ volatile("lea %1, %0" : "=r"(gdt_addr) : "m"(gdt[0]));  // RIP-relative!
    /* raw debug: GA<base32>-<lim> */
    { out8(0x3F8,'G'); out8(0x3F8,'A'); out8(0x3F8,':');
      for(int sh=28; sh>=0; sh-=4){ int n=((u32)gdt_addr>>sh)&0xF; out8(0x3F8, n<10?'0'+n:'A'+n-10); }
      out8(0x3F8,'-');
      for(int sh=12; sh>=0; sh-=4){ int n=(71>>sh)&0xF; out8(0x3F8, n<10?'0'+n:'A'+n-10); }
      out8(0x3F8,'\n'); }
    struct { u16 lim; u64 base; } __attribute__((packed)) gdtr = {
        sizeof(gdt)-1, gdt_addr
    };
    __asm__ volatile("lgdt %0" :: "m"(gdtr));
    __asm__ volatile(
        "mov %0, %%ax;"
        "mov %%ax, %%ds; mov %%ax, %%es; mov %%ax, %%ss;"
        "push %1; lea 1f(%%rip), %%rax; push %%rax; lretq; 1:"
        :: "i"(0x10), "i"(0x08) : "rax", "memory"
    );
}

/* set kernel stack for interrupts coming from Ring 1 */
static void tss_set_rsp0(u64 rsp0) {
    tss_rsp0 = rsp0;
    *(u64*)(tss + 4) = rsp0;   // RSP0 field in TSS
    __asm__ volatile("ltr %%ax" :: "a"((u16)0x30));   /* TSS now at gdt[6] (0x30) */
}

/* Ring1 user stack for call-gate entry (64-bit TSS has RSP1, no RSP3!) */
static void tss_set_rsp1(u64 rsp1) {
    *(u64*)(tss + 12) = rsp1;  // RSP1 field in TSS
}

/* jump to user code via SYSRET (iretq to Ring1/3 #GPs on QEMU 10/11 —
   verified with a clean multiboot kernel: sysret works, iretq doesn't).
   STAR user base 0x18 → sysret CS=0x2B (gdt[5]), SS=0x23 (gdt[4]).
   DS/ES must be loaded by user code itself (Ring3 can load DPL3 seg). */
static void jump_user(void (*user_fn)(void), int ring) {
    static u8 user_stack[16384] __attribute__((aligned(16)));
    u64 fn, rsp;
    __asm__ volatile("lea %1, %0" : "=r"(fn) : "m"(*user_fn));
    __asm__ volatile("lea %1, %0" : "=r"(rsp) : "m"(user_stack[0]));
    rsp += sizeof(user_stack) - 8;
    /* EFER.SCE = 1 */
    __asm__ volatile("movl $0xC0000080, %%ecx; rdmsr; orl $1, %%eax; wrmsr"
                     ::: "eax", "ecx", "edx", "memory");
    /* STAR = user CS base 0x18 | kernel CS 0x08 */
    __asm__ volatile("movl $0xC0000081, %%ecx; xorl %%eax, %%eax;"
                     "movl $0x00180008, %%edx; wrmsr"
                     ::: "eax", "ecx", "edx", "memory");
    __asm__ volatile(
        "movq %0, %%rcx;"      /* user_fn */
        "movq %1, %%rsp;"      /* user stack — sysret does NOT set RSP! */
        "movq $0x202, %%r11;"  /* RFLAGS IF=1 — test IRQ-return iretq to Ring3 */
        "sysretq"
        :: "r"(fn), "r"(rsp) : "rcx", "r11", "rsp", "memory");
    
    for(;;);  /* not reached */
}
