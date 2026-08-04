/* desk/task.h — minimal round-robin scheduler */
#pragma once
#include "../boot/types.h"
#include "../drivers/pid.h"
#include "../sys/serial.h"

#define MAX_TASKS 8
#define TASK_STACK 4096

typedef void (*task_fn)();

struct Task {
    pid_t pid;
    u8 *stack;
    u64 rsp;       // saved stack pointer
    int state;     // 0=free, 1=ready, 2=running
    int priority;  // 1=low, 2=normal, 3=high
    task_fn entry;
    const char *name;
};

static Task tasks[MAX_TASKS];
static int cur_task = -1;
static u8 *task_stacks;  // stack memory pool

// Assembly helper: switch stacks
static void __attribute__((naked)) task_switch_to(u64 *new_rsp) {
    __asm__ volatile(
        "push %%rbp; mov %%rsp, %%rbp;"
        "mov 16(%%rbp), %%rax;"   // new_rsp
        "push %%rbx; push %%r12; push %%r13; push %%r14; push %%r15;"
        "mov %%rsp, (%%rax);"     // save current rsp → *new_rsp
        "mov 8(%%rbp), %%rax;"    // new stack
        "mov %%rax, %%rsp;"       // switch
        "pop %%r15; pop %%r14; pop %%r13; pop %%r12; pop %%rbx;"
        "pop %%rbp; ret;"
        ::: "memory"
    );
}

static void task_init() {
    // Allocate stack pool in BSS
    static u8 _stacks[MAX_TASKS * TASK_STACK] __attribute__((aligned(16)));
    task_stacks = _stacks;
    for(int i=0;i<MAX_TASKS;i++) tasks[i].state=0;
}

static pid_t task_create(task_fn entry, const char *name, int prio) {
    for(int i=0;i<MAX_TASKS;i++) if(tasks[i].state==0) {
        Task *t = &tasks[i];
        t->pid = pid_alloc();
        t->stack = task_stacks + i * TASK_STACK;
        t->entry = entry;
        t->name = name;
        t->priority = prio;
        t->state = 1;
        // Set up initial stack frame so task_switch_to can pop into entry
        u64 *sp = (u64*)(t->stack + TASK_STACK - 8);
        *(--sp) = (u64)entry;  // "return address"
        *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; *(--sp) = 0; // rbx/r12-r15
        *(--sp) = 0; // rbp
        t->rsp = (u64)sp;
        out_str("Task: "); out_str(name); out_str("\n");
        return t->pid;
    }
    return 0;
}

static void task_yield() {
    int next = (cur_task + 1) % MAX_TASKS;
    while(next != cur_task && tasks[next].state != 1) next = (next+1)%MAX_TASKS;
    if(next == cur_task) return; // no other ready task
    Task *t = &tasks[cur_task];
    Task *n = &tasks[next];
    t->state = 1;
    n->state = 2;
    u64 old_rsp = t->rsp;
    cur_task = next;
    task_switch_to(&old_rsp);
    t->rsp = old_rsp;
}
