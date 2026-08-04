/* sys/exception.h — exception handlers → dead screen */
#pragma once
extern "C" volatile u32 user_marker;  /* iretq marker */
#include "idt.h"
#include "../security/dead_screen.h"

static const char *exc_names[] = {
    "DIVIDE_BY_ZERO","DEBUG","NMI","BREAKPOINT","OVERFLOW",
    "BOUND_RANGE","INVALID_OPCODE","DEVICE_NOT_AVAILABLE","DOUBLE_FAULT",
    "COPROCESSOR_SEGMENT_OVERRUN","INVALID_TSS","SEGMENT_NOT_PRESENT",
    "STACK_SEGMENT_FAULT","GENERAL_PROTECTION_FAULT","PAGE_FAULT",
    nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
    nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
    "X87_FLOATING_POINT","ALIGNMENT_CHECK","MACHINE_CHECK","SIMD_FLOATING_POINT"
};

/* frame: [0]=vec, [1..15]=regs (rax..r15), [16]=original RIP */
/* stub pushes: num, rax, rcx, rdx, rbx, rbp, rsi, rdi, r8-r15 (r15 LAST, RDI points at it)
   → vec (num) sits at offset 15*8=120, NOT offset 0! Old struct read r15 as vec. */
struct ExcFrame {
    u64 r15, r14, r13, r12, r11, r10, r9, r8,
        rdi, rsi, rbp, rbx, rdx, rcx, rax, vec;
};

extern "C" void exc_handler(ExcFrame *f) {
    const char *name = (f->vec < 32 && exc_names[f->vec]) ? exc_names[f->vec] : "UNKNOWN";
    char dbg[96];
    ksprintf(dbg, "EXC %d (%s) F=%x", (int)f->vec, name, (u32)(u64)f);
    out_file_str(dbg);
    out_file_str("\n");
    /* dump raw frame: [0]=vec [1..15]=regs [16]=ret */
    for(int i=0;i<18;i++){
        u64 v=((u64*)f)[i];
        char h[48];
        ksprintf(h, " [%d]=%x", i, (u32)v);
        out_file_str(h);
    }
    out_file_str("\n");
    { char dbg[40]; ksprintf(dbg,"MARKER %x",(u32)user_marker); out_file_str(dbg); out_file_str("\n"); }
    if (f->vec == 14) {  /* #PF: print CR2 */
        u64 cr2v; __asm__ volatile("movq %%cr2, %0" : "=r"(cr2v));
        char dbg2[48]; ksprintf(dbg2,"[KERNEL/INFO] CR2=%x",(u32)cr2v); out_file_str(dbg2); out_file_str("\n");
    }
    /* full raw dump: f[15]=vec, f[16..22]=CPU frame (errcode/RIP/CS/RFLAGS[+SS/RSP])
       then the iretq frame (RIP,CS,RFLAGS,RSP,SS) below it */
    { u64 *raw = (u64*)f;
      for(int i=15;i<29;i++){ char h[32]; ksprintf(h,"[%d]=%x ",i,(u32)raw[i]); out_file_str(h); }
      out_file_str("\n"); }
    /* ★ 蓝屏显示有用信息：异常名 + RIP + 错误码（不再只显示 KERNEL_EXCEPTION）
       有错误码的异常（8,10-14,17）：[16]=errcode [17]=RIP；无：[16]=RIP [17]=CS */
    {
        int has_err = (f->vec==8 || (f->vec>=10 && f->vec<=14) || f->vec==17);
        u64 *raw = (u64*)f;
        u64 rip = raw[has_err ? 17 : 16];
        u64 err = raw[16];
        /* Windows 风格停止代码（异常 → Win 蓝屏代码）——直接用 Windows 的 */
        static const char *win_stopcodes[] = {
            "DIVIDE_BY_ZERO","KERNEL_EXCEPTION","NMI_HARDWARE_FAILURE","BREAKPOINT",
            "OVERFLOW","KERNEL_EXCEPTION","KERNEL_EXCEPTION","DEVICE_NOT_AVAILABLE",
            "DOUBLE_FAULT","KERNEL_EXCEPTION","KERNEL_STACK_INPAGE_ERROR","SEGMENT_NOT_PRESENT",
            "KERNEL_STACK_INPAGE_ERROR","KERNEL_EXCEPTION","PAGE_FAULT_IN_NONPAGED_AREA",
            nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
            nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,nullptr,
            "X87_FLOATING_POINT","ALIGNMENT_CHECK","MACHINE_CHECK_EXCEPTION","KERNEL_EXCEPTION"
        };
        const char *win = (f->vec < 32 && win_stopcodes[f->vec]) ? win_stopcodes[f->vec] : "KERNEL_EXCEPTION";
        ds(win);
    }
}

extern "C" void exc0(), exc1(), exc2(), exc3(), exc4(), exc5(), exc6(), exc7();
extern "C" void exc8(), exc9(), exc10(), exc11(), exc12(), exc13(), exc14();
extern "C" void exc16(), exc17(), exc18(), exc19();

// Register exception handlers (0-31) to dead screen
static void exc_init() {
    void (*tbl[32])() = {0};
    tbl[0]=exc0; tbl[1]=exc1; tbl[2]=exc2; tbl[3]=exc3;
    tbl[4]=exc4; tbl[5]=exc5; tbl[6]=exc6; tbl[7]=exc7;
    tbl[8]=exc8; tbl[9]=exc9; tbl[10]=exc10; tbl[11]=exc11;
    tbl[12]=exc12; tbl[13]=exc13; tbl[14]=exc14;
    tbl[16]=exc16; tbl[17]=exc17; tbl[18]=exc18; tbl[19]=exc19;
    for(int i=0;i<32;i++) if(tbl[i]) set_gate(i, (void*)tbl[i]);
}
