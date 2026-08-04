/* WinDOS.h — kernel API */
#pragma once
#include "boot/types.h"

/* Colors: write 0xRRGGBB values (B low byte → framebuffer BGR byte0) */
const u32 black = 0x00000000;
const u32 white = 0x00FFFFFF;
const u32 red = 0x00FF0000;
const u32 green = 0x0000FF00;
const u32 blue = 0x000000FF;
const u32 yellow = 0x00FFFF00;
const u32 trans = 0x00FF00FF;
typedef u32 pid_t;

// Process
pid_t GetCurrentProcess();
void ExitProcess(int code);
void TerminateProcess(pid_t pid, int code);

// Display
void GetDesktopResolution(u32 *w, u32 *h);
// Memory
void *kmalloc(u32 size);
void kfree(void *ptr);
#define AllocMem kmalloc
#define FreeMem kfree
// System
u64 GetTickCount();
void Sleep(u32 ms);
// Debug
void DebugPrint(const char *s);
void FatalError(const char *msg);
// Time (add later)
/*
 * u128 GetCurrentTime();
 *...
 */
// Sheet (Add later)
/*
 * void PauseAutoRefresh();
 * ...
 */
/* ── Win32-style syscall API (Ring3 user programs) ──
   syscall ABI: rax=num, rdi,rsi,rdx,r10,r8,r9 (rcx clobbered by CPU) */
static inline void WriteConsole(const char *s) {
    __asm__ volatile(
        "movq $1, %%rax; movq %0, %%rdi; syscall"
        :: "r"((u64)s) : "rax", "rdi", "rcx", "r11", "memory");
}
static inline void DrawRect(int x, int y, int w, int h, u32 color) {
    __asm__ volatile(
        "movq $2, %%rax; movq %0, %%rdi; movq %1, %%rsi; movq %2, %%rdx;"
        "movq %3, %%r10; movq %4, %%r8; syscall"
        :: "r"((u64)x), "r"((u64)y), "r"((u64)w), "r"((u64)h), "r"((u64)color)
        : "rax", "rdi", "rsi", "rdx", "r10", "r8", "rcx", "r11", "memory");
}
static inline int CreateWindow(int x, int y, int w, int h, const char *title) {
    u64 ret;
    __asm__ volatile(
        "movq $3, %%rax; movq %1, %%rdi; movq %2, %%rsi; movq %3, %%rdx;"
        "movq %4, %%r10; movq %5, %%r8; syscall; movq %%rax, %0"
        : "=r"(ret)
        : "r"((u64)x), "r"((u64)y), "r"((u64)w), "r"((u64)h), "r"((u64)title)
        : "rax", "rdi", "rsi", "rdx", "r10", "r8", "rcx", "r11", "memory");
    return (int)ret;
}
static inline void FillRect(int win, int x, int y, int w, int h, u32 color) {
    u64 w0 = win, x0 = x, y0 = y, w1 = w, h1 = h, c0 = color;
    __asm__ volatile(
        "movq $4, %%rax; movq %0, %%rdi; movq %1, %%rsi; movq %2, %%rdx;"
        "movq %3, %%r10; movq %4, %%r8; movq %5, %%r9; syscall"
        :: "m"(w0), "m"(x0), "m"(y0), "m"(w1), "m"(h1), "m"(c0)
        : "rax", "rdi", "rsi", "rdx", "r10", "r8", "r9", "rcx", "r11", "memory");
}
static inline void DestroyWindow(int win) {
    __asm__ volatile(
        "movq $5, %%rax; movq %0, %%rdi; syscall"
        :: "r"((u64)win) : "rax", "rdi", "rcx", "r11", "memory");
}

/* SYS_POLL_INPUT(6): process pending mouse packets (keeps cursor alive in user loop) */
static inline void PollInput(void) {
    __asm__ volatile(
        "movq $6, %%rax; syscall"
        :: : "rax", "rcx", "r11", "memory");
}
