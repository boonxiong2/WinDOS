/* dead_screen.h — A security screen when a problem has been detected in System level */
#pragma once
#include "../sys/stdkern.h"
/* Win 风格蓝屏。Error = Windows 停止代码（如 KERNEL_EXCEPTION / PAGE_FAULT_IN_NONPAGED_AREA） */
static void ds(const char *Error) {
    /* 崩溃处理程序自身必须安全，两条：
       (1) 不能信 g_fb/g_stride/disp_w()/disp_h()——它们就在 .bss 里紧挨着大缓冲，
           往往正是被踩坏的那批数据；g_fb 变野值会让 fill_screen 二次崩溃
       (2) 必须防重入——崩→ds()→再崩→ds()……递归到栈耗尽，把真正的现场盖掉 */
    static volatile int entered = 0;
    out_file_str("[KERNEL/CRITICAL/INFO]BSOD DETECTED");
    io_cli();
    if (entered) {
        for (;;) { io_cli(); io_hlt(); }   /* 已在崩——绝不递归 */
    }
    entered = 1;
    {
        u32 *fb = g_fb;
        u32 st = g_stride, W = disp_w(), H = disp_h();
        u64 fbv = (u64)fb;
        /* 显存指针 + 几何合法性检查：规范地址、4 字节对齐、宽高/步幅在合理范围 */
        int fb_ok = (((fbv >> 47) == 0) || ((fbv >> 47) == 0x1FFFF))
                    && (fbv & 3) == 0
                    && W > 0 && W <= 8192 && H > 0 && H <= 8192
                    && st >= W && st <= 8192;
        if (fb_ok) {
            fill_screen(fb, st, W, H, 0x00000000);   /* Win11 风格：纯黑屏（"蓝屏"改黑了） */
            /* Win11 风格排版：标题 + 进度（未实现） + 停止代码 */
            put_str(fb, st, W/2-40 - 170, H/2, "Your device ran into a problem and needs to restart.", 0x00FFFFFF);
            char buf[160];
            sprintf(buf, "Stop code: %s", Error);
            put_str(fb, st, W/2-130, H-20, buf, 0x00FFFFFF);
            out_file_str("[KERNEL/CRITICAL/INFO]BSOD DREW");
        } else {
            out_file_str("[KERNEL/CRITICAL/INFO]BSOD SKIPPED (display state unusable)");
        }
    }
    for (;;) {
        io_cli();
        io_hlt();
    }
}
