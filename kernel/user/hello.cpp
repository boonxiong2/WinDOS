/* user/hello.cpp — runs in Ring 3 (user mode) */
#include "../sys/log.h"
#include "../sys/serial.h"
#include "../boot/types.h"
#include "../WinDOS.h"   /* Win32-style API lives here */

extern "C" void user_main(void);
extern "C" volatile u32 user_marker;  /* sysret marker */

/* ═══════════════════════════════════════════════════
 * hello.cpp — 用户态（Ring 3）程序
 * 运行方式：登录界面输入用户名按回车 → jump_user(user_main) →
 *           sysret 指令降到 Ring 3 执行 user_main
 * 当前内容：
 *   1. 写 user_marker（验证确实在用户态——%ss: 前缀绕开 DS 限制）
 *   2. syscall 1 打印 "HELLO FROM RING3"（验证 syscall 通路）
 *   3. 死循环空转——鼠标/窗口全由内核中断(isr2c)处理，用户程序不用管
 * ═══════════════════════════════════════════════════ */
extern "C" void user_main(void) {
    /* 注意：QEMU 上 Ring3 不能 mov 到 DS（#GP）——访问数据用 %ss: 前缀
       （SS=0x23 是 sysret 合成的用户数据段，DPL=3 合法） */
    __asm__ volatile("movl $0xDEADBEEF, %%ss:%0" :: "m"(user_marker) : "memory");
    /* syscall test: SYS_WRITE(1, "HELLO FROM RING3\n") — kernel prints to COM1.
       syscall preserves RAX/RDI (RCX/R11 clobbered by CPU). */
    __asm__ volatile(
        "movq $1, %%rax;"
        "leaq msg(%%rip), %%rdi;"
        "syscall"
        :: : "rax", "rdi", "rcx", "r11", "memory");
    /* idle loop: mouse is handled by isr2c (kernel), so no polling API —
       just spin; PIT IRQs (IF=1) keep firing: isr20 → sysretq back. */
    volatile u64 n = 0;
    for(;;) {
        n++;
    }
}
/* user .rodata — string for SYS_WRITE (kernel reads it in Ring0, DS ok) */
extern "C" const char msg[] = "HELLO FROM RING3\n";
