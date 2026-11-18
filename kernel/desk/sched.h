/* desk/sched.h — 协作式轮转调度器（最小可用版）
   与旧 desk/task.h 的区别：切换代码按 SysV ABI 正确保存 callee-saved 寄存器
   （rbx/rbp/r12-r15），任务栈帧布局与 sched_switch 的 pop 顺序严格对应。

   约定：
     - task 0 = main（调用 sched_init() 的那条执行流），它的 rsp 在第一次
       sched_yield() 时被保存
     - 任务里必须显式 sched_yield()（协作式，不做抢占）
     - 任务函数返回前必须调用 sched_exit()；直接 return 会跳到垃圾地址
     - sched_kill(pid)：把任务标记为 free，之后不再被调度（供 TerminateProcess 用）
*/
#pragma once
#include "../boot/types.h"
#include "../sys/serial.h"

#define SCHED_MAX    8
#define SCHED_STACK  8192

struct Task {
    u64  rsp;        /* 保存的栈指针 */
    u8  *stack;      /* 栈底（sched_create 分配）*/
    int  state;      /* 0=free 1=ready 2=running */
    u32  pid;
    const char *name;
};

static Task g_tasks[SCHED_MAX];
static int  g_cur = -1;
static int  g_sched_ready = 0;
static u32  g_next_pid = 1;

/* 保存当前上下文 → *save；切换到 next 的上下文
   AT&T 语法；参数：rdi=&save, rsi=next_rsp */
static void __attribute__((naked)) sched_switch(u64 *save, u64 next) {
    __asm__ volatile(
        "push %rbx\n\t"
        "push %rbp\n\t"
        "push %r12\n\t"
        "push %r13\n\t"
        "push %r14\n\t"
        "push %r15\n\t"
        "mov %rsp, (%rdi)\n\t"   /* 保存旧 rsp */
        "mov %rsi, %rsp\n\t"     /* 换栈 */
        "pop %r15\n\t"
        "pop %r14\n\t"
        "pop %r13\n\t"
        "pop %r12\n\t"
        "pop %rbp\n\t"
        "pop %rbx\n\t"
        "ret\n\t"
    );
}

static void sched_init(void) {
    for (int i = 0; i < SCHED_MAX; i++) {
        g_tasks[i].state = 0; g_tasks[i].rsp = 0;
        g_tasks[i].stack = 0; g_tasks[i].name = 0; g_tasks[i].pid = 0;
    }
    /* task 0 = main：rsp 在第一次 yield 时填 */
    g_tasks[0].state = 2;
    g_tasks[0].name = "main";
    g_tasks[0].pid = 0;
    g_cur = 0;
    g_sched_ready = 1;
}

/* 建任务：返回 pid（0 = 失败）*/
static u32 sched_create(void (*entry)(void), const char *name) {
    if (!g_sched_ready) return 0;
    static u8 stacks[(SCHED_MAX - 1) * SCHED_STACK] __attribute__((aligned(16)));
    for (int i = 1; i < SCHED_MAX; i++) {
        if (g_tasks[i].state != 0) continue;
        Task *t = &g_tasks[i];
        t->stack = stacks + (i - 1) * SCHED_STACK;
        t->name = name;
        t->pid = g_next_pid++;
        t->state = 1;
        /* 栈帧（从低到高）：r15 r14 r13 r12 rbp rbx ret —— 与 sched_switch 的 pop 对应 */
        u64 *sp = (u64*)(t->stack + SCHED_STACK);
        *(--sp) = (u64)entry;   /* ret 目标 */
        *(--sp) = 0;            /* rbx */
        *(--sp) = 0;            /* rbp */
        *(--sp) = 0;            /* r12 */
        *(--sp) = 0;            /* r13 */
        *(--sp) = 0;            /* r14 */
        *(--sp) = 0;            /* r15 */
        t->rsp = (u64)sp;
        return t->pid;
    }
    return 0;
}

/* 让出 CPU：优先给下一个 ready 任务，没有就回 main */
static void sched_yield(void) {
    if (!g_sched_ready) return;
    int prev = g_cur;
    int next = -1;
    for (int k = 1; k < SCHED_MAX; k++) {
        int idx = (prev + k) % SCHED_MAX;
        if (idx == 0) continue;
        if (g_tasks[idx].state == 1) { next = idx; break; }
    }
    if (next < 0) {
        if (prev == 0) return;          /* main 且没人可跑 → 直接返回 */
        next = 0;                        /* 回 main */
    }
    if (g_tasks[prev].state == 2) g_tasks[prev].state = 1;
    if (prev != 0 && g_tasks[prev].state == 0) { /* 已被 kill：不回队列 */ }
    if (g_tasks[next].state == 0) return;
    g_tasks[next].state = 2;
    g_cur = next;
    sched_switch(&g_tasks[prev].rsp, g_tasks[next].rsp);
}

/* 任务结束：标记 free 并让出（不会返回）*/
static void sched_exit(void) {
    int me = g_cur;
    if (me > 0) g_tasks[me].state = 0;
    sched_yield();
    for (;;) __asm__ volatile("hlt");   /* 理论上到不了 */
}

/* 结束某个任务（TerminateProcess 用）。返回 0 成功，-1 没这个任务 */
static int sched_kill(u32 pid) {
    for (int i = 1; i < SCHED_MAX; i++) {
        if (g_tasks[i].pid != pid || g_tasks[i].state == 0) continue;
        g_tasks[i].state = 0;              /* 不再被调度 */
        return 0;
    }
    return -1;
}

static int sched_alive(void) {
    int n = 0;
    for (int i = 1; i < SCHED_MAX; i++) if (g_tasks[i].state) n++;
    return n;
}
