/* dead_screen.h — A security screen when a problem has been detected in System level */
#pragma once
#include "../sys/stdkern.h"
/* Win 风格蓝屏。Error = Windows 停止代码（如 KERNEL_EXCEPTION / PAGE_FAULT_IN_NONPAGED_AREA） */
static void ds(const char *Error) {
    out_file_str("[KERNEL/CRITICAL/INFO]BSOD DETECTED");
    io_cli();
    fill_screen(g_fb, g_stride, disp_w(), disp_h(), 0x00000000);   /* Win11 风格：纯黑屏（"蓝屏"改黑了） */
    int W = disp_w(), H = disp_h();
    /* Win11 风格排版：标题 + 进度（未实现） + 停止代码*/
    put_str(g_fb, g_stride, W/2-40 - 170, H/2, "Your device ran into a problem and needs to restart.", 0x00FFFFFF);
    char buf[160];
    sprintf(buf, "Stop code: %s", Error);
    put_str(g_fb, g_stride, W/2-130, H-20, buf, 0x00FFFFFF);
    out_file_str("[KERNEL/CRITICAL/INFO]BSOD DREW");
    for (;;) {
        io_cli();
        io_hlt();
    }
}
