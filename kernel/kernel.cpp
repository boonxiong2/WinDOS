/* ============================================================
 * kernel.cpp — WinDOS 内核主文件（UEFI 64 位，clang 编译）
 * 职责：启动初始化（页表/中断/键盘鼠标）、图层窗口系统、登录界面、
 *       跳转用户态（Ring3 sysret）、系统调用分发（syscall）
 * 图层系统见 window.h（原版 haribote sheet.c 移植）
 * 鼠标处理见 sys/isr.h（isr2c 中断内直接处理）
 * ============================================================ */
#include "sys/stdkern.h"
#include "sys/panic.h"
#include "build_info.h"   /* build.bat 生成——编译时间戳 */
#include "fs/exfat.h"
#include "fs/nvme.h"

struct BootInfo { u64 fb_base, fb_size; u32 hr, vr, stride, px_fmt; u16 tm_year; u8 tm_mon, tm_mday, tm_hour, tm_min, tm_sec;
                  /* ── 引导盘位置：UEFI（引导器）填的，不是内核猜的 ── */
                  u32 ctrl_kind, pci_addr, part_lba, part_size; };

IdtEntry idt[256];
Fifo mfifo, kfifo;



// 鼠标光标形状（硬编码字符画）：b=黑色边缘 o=白色填充 /=透明
// 每个字符 = 一个像素（16x18 的箭头光标）
static const char cs[16][19] = {
    "b/////////////////",
    "bb////////////////",
    "bob///////////////",
    "boob//////////////",
    "booob/////////////",
    "boooob////////////",
    "booooob///////////",
    "boooooob//////////",
    "booooooob/////////",
    "boooooooob////////",
    "booboobbbbb///////",
    "bobboob///////////",
    "bb/boob///////////",
    "b///boob//////////",
    "////boob//////////",
    "/////bb///////////"
};


// 窗口关闭按钮（X）形状（14x16 字符画）：O=边框 $=阴影 @=黑色X Q=白色
// draw_win 里把这个按钮画到窗口右上角
static const char closebtn[14][17]={
    "OOOOOOOOOOOOOOOO",
    "OQQQQQQQQQQQQQ$O",
    "OQQQQQQQQQQQQQ$O",
    "OQQQ@QQQQQ@QQQ$O",
    "OQQQQ@QQQ@QQQQ$O",
    "OQQQQQ@Q@QQQQQ$O",
    "OQQQQQQ@QQQQQQ$O",
    "OQQQQQ@Q@QQQQQ$O",
    "OQQQQ@QQQ@QQQQ$O",
    "OQQQ@QQQQQ@QQQ$O",
    "OQQQQQQQQQQQQQ$O",
    "OQQQQQQQQQQQQQ$O",
    "O$$$$$$$$$$$$$$O",
    "OOOOOOOOOOOOOOOO"
};
// ── Cursor direct draw (no sheet) ──
static void cursor_draw(u32 *fb, u32 st, int x, int y) {
}
static void cursor_erase(u32 *fb, u32 stride, Cursor *c, u32 hr, u32 vr) {
    // 卧槽这代码不注释整个汐统直接崩
    /*
    for (u32 dy = 0; dy < 16; dy++) {
        u32 bg = ((u32)c->y + dy >= vr - 40) ? 0 : 0x00008080;
        for (u32 dx = 0; dx < 18; dx++) {
            int px = c->x + (int)dx;
            if (px < 0 || (u32)px >= hr) continue;
            fb[(c->y + dy) * stride + px] = bg;
        }
    }
    */
}
// 画一个窗口到像素缓冲 buf：
//   1. 整窗填充浅灰背景(0xC0C0C0)
//   2. 顶部 24px 画标题栏（白底+深灰边框）
//   3. 写标题文字 + 右上角关闭按钮
// 注意：只画进 buf（图层缓冲），显示到屏幕靠图层刷新（sheet_refresh）
struct SHEET *g_shadow_shts[8];   /* ★ 阴影层列表（半透明合成）——刷新联动（window.h extern） */
int g_shadow_cnt;

static void draw_win(u32 *buf, int w, int h, const char *title) {
    for(int y=0;y<h;y++)for(int x=0;x<w;x++)buf[y*w+x]=0x00C0C0C0;
    for(int y=0;y<24;y++)for(int x=0;x<w;x++)buf[y*w+x]=(y<2||x<2||x>=w-2)?0x00404040:0x00FFFFFF;  // white title bar
    put_str(buf,w,5,4,title,0x00000000);  // black title text
    // Close button (14x16) at top-right: position xsize-21, y=5
    for(int y=0;y<14;y++)for(int x=0;x<16;x++){
        char p=closebtn[y][x];
        u32 col=0x00FFFFFF;  // white
        if(p=='@') col=0x00000000;  // black X
        buf[(5+y)*w+(w-21+x)]=col;
    }
}

/* 带渐变阴影的窗口——Win11 式"贴边投影"：
   窗口内容不偏移；阴影围绕四边，从窗口边缘（0 距离）向外渐变淡出
   （近边深 α=0x50 → 外边 α=0——半透明——需要图层系统 alpha 混合） */
#define SHADOW 16  /* 阴影边距（continue 坑已修——16 安全） */

static void draw_win_shadow(u32 *buf, int w, int h, const char *title) {
    int W = w + 2 * SHADOW, H = h + 2 * SHADOW;
    /* 内容区（居中——四周留阴影带）——原 draw_win 逻辑 */
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) buf[(y + SHADOW) * W + (x + SHADOW)] = 0x00C0C0C0;
    for (int y = 0; y < 24; y++)
        for (int x = 0; x < w; x++)
            buf[(y + SHADOW) * W + (x + SHADOW)] = (y < 2 || x < 2 || x >= w - 2) ? 0x00404040 : 0x00FFFFFF;
    put_str(buf, W, 5 + SHADOW, 4 + SHADOW, title, 0x00000000);
    for (int y = 0; y < 14; y++)
        for (int x = 0; x < 16; x++) {
            char p = closebtn[y][x];
            buf[(5 + y + SHADOW) * W + (w - 21 + x + SHADOW)] = (p == '@') ? 0x00000000 : 0x00FFFFFF;
        }
    /* ★ 贴边投影：四边从窗口边缘（0 距离）向外渐变——近深 α=0x50 → 外 α=0
       CRITICAL: 不用 continue（clang -O2 会优化成跳循环尾不递增→死循环/E06——
       之前踩过同款坑）；加 unroll(disable) 防步长展开 */
    #pragma clang loop unroll(disable)
    for (int y = 0; y < H; y++) {
#pragma clang loop unroll(disable)
        for (int x = 0; x < W; x++) {
            int dx, dy;
            /* 三元表达式——clang -O2 陷阱（5706 E06/死循环）——改 if/else */
            if (x < SHADOW) dx = SHADOW - x;
            else if (x >= SHADOW + w) dx = x - (SHADOW + w - 1);
            else dx = 0;
            if (y < SHADOW) dy = SHADOW - y;
            else if (y >= SHADOW + h) dy = y - (SHADOW + h - 1);
            else dy = 0;
            if (dx != 0 || dy != 0) {           /* if 包住——不用 continue */
                int d = dx > dy ? dx : dy;
                if (d > SHADOW) d = SHADOW;
                u32 depth = SHADOW - d + 3;             /* 近窗 depth=16 → 外 depth=1 */
                buf[y * W + x] = 0xFF000000 | depth;    /* 合成标记(高位) + 层数 */

            }
        }
    }
}

extern "C" volatile u32 user_marker = 0;  /* set by user_main if sysret succeeded */
extern "C" void syscall_entry_asm(void);  /* isr.S */

/* 全局鼠标/窗口状态 — 鼠标处理在 isr2c 中断里（原样复制内核主循环的鼠标分支，
   这样登录界面和用户态下窗口都能拖拽）。这些变量供 isr.h 里的中断处理使用。
   g_shtctl   = 图层管理器指针（窗口/光标的"世界"）
   g_m        = 鼠标包组装状态（3 字节一包）
   g_mx/g_my  = 光标当前屏幕坐标
   g_cur_sht  = 光标图层
   g_mmx/g_mmy/g_drag_sht = 拖拽状态（按下左键时的记录）
   g_login_sht = 登录窗口（不可关闭——判断用） */
SHTCTL *g_shtctl;
MP g_m; int g_mx, g_my; struct SHEET *g_cur_sht;
int g_mmx = -1, g_mmy = -1; struct SHEET *g_drag_sht = 0;
struct SHEET *g_login_sht = 0;

/* 系统调用分发器：用户态程序用 syscall 指令进内核（入口在 isr.S 的
   syscall_entry_asm），CPU 自动把调用号放进 RAX、参数放进 RDI/RSI/...
   这里根据 RAX 分发：
     1 = SYS_WRITE —— 内核把用户字符串打印到串口(COM1)
   其他调用号 → 返回 -1（错误，不静默放过）
   注意：不能用 register-asm 变量读寄存器（编译器会优化掉 if），
   所以用显式 movq 读 RAX/RDI。 */
extern "C" u8 _bss_end;   /* 用户可访问内存上界（按它推导——见下面 US 位翻转） */
extern "C" u64 syscall_dispatch(void) {
    /* register-asm variables are UB (compiler optimizes the if away) —
       read regs explicitly with mov */
    u64 num, a1;
    __asm__ volatile("movq %%rax, %0" : "=r"(num));
    __asm__ volatile("movq %%rdi, %0" : "=r"(a1));
    if (num == 1) {
        /* 用户可访问区 = 已翻 US=1 的低内存（与上面 US 位翻转同一来源）。
           不能写死 8MB：.bss 涨到 10.6MB 后用户字符串/栈都在 8MB 以上，
           写死上限会把合法指针全判成越界。按 _bss_end 向上取整到 2MB 页。 */
        u64 be; __asm__ volatile("lea %1, %0" : "=r"(be) : "m"(_bss_end));
        u64 lim = (be + 0x1FFFFF) & ~0x1FFFFFull;
        if ((u64)a1 < 0x1000 || (u64)a1 >= lim) {out_file_str("[KERNEL/USER_API/EXCPTION]\
                             Application used Pointer DIDN'T in user mem area/");return -1;  /* 指针必须在用户区 */}
        out_file_str((char*)a1);
        return 0;
    }
    return (u64)-1;          /* unknown syscall → error (no silent pass-through) */
}
unsigned char keystatus[256];
volatile u32 g_ticks;

// 键盘扫描码 → 字符表（原版 WinDOS 的）：
//   keytable0 = 普通键（无 Shift）  keytable1 = Shift 组合键
//   索引 = 扫描码（Set1 键盘），值是字符或 0（无对应字符）
//   '0x08'=退格  '0x0a'=回车
static char keytable0[0x80] = {
    0,   0,   '1', '2', '3', '4', '5', '6', '7', '8', '9', '0', '-', '^', 0x08, 0,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '@', '[', 0x0a, 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', ';', ':', 0,   0,   ']', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', ',', '.', '/', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0x5c, 0,  0,   0,   0,   0,   0,   0,   0,   0,   0x5c, 0,  0
};
static char keytable1[0x80] = {
    0,   0,   '!', 0x22, '#', '$', '%', '&', 0x27, '(', ')', '~', '=', '~', 0x08, 0,
    'Q', 'W', 'E', 'R', 'T', 'Y', 'U', 'I', 'O', 'P', '`', '{', 0x0a, 0, 'A', 'S',
    'D', 'F', 'G', 'H', 'J', 'K', 'L', '+', '*', 0,   0,   '}', 'Z', 'X', 'C', 'V',
    'B', 'N', 'M', '<', '>', '?', 0,   '*', 0,   ' ', 0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   '7', '8', '9', '-', '4', '5', '6', '+', '1',
    '2', '3', '0', '.', 0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,
    0,   0,   0,   '_', 0,   0,   0,   0,   0,   0,   0,   0,   0,   '|', 0,   0
};

constexpr int MAX_INT = 2147483647;
extern "C" void user_main(void);

extern "C" u8 _bss_start, _bss_end;
u64 KERN_BASE = 0;

/* ============================================================
 * _start —— 内核入口（bootloader 跳进来，传 BootInfo*：framebuffer 信息）
 * 顺序：关中断 → 记 CR0/CR3 → 清 BSS → 装自己的 GDT → 改页表 US 位
 *       → 设 syscall 入口(LSTAR) → 初始化显示 → 建图层/窗口/登录界面
 *       → 进主循环（等键盘输入登录）
 * ============================================================ */
extern "C" __attribute__((section(".text.start"))) void _start(BootInfo *info) {
    /* CRITICAL: disable interrupts IMMEDIATELY — between gdt_init() and
       lidt_idt() the firmware timer IRQ can fire and jump through the
       FIRMWARE IDT with OUR GDT loaded → random #GP/#PF crash at a
       different spot every boot! */
    __asm__ volatile("cli");
    {
        /* UMIP（防 Ring3 用 SIDT 拿 IDT 地址——CIH 提权手法）。
           ★ 必须先查 CPUID：QEMU 默认 CPU 不支持 UMIP——
           直接写 CR4 置位会 #GP（启动即崩——踩过！） */
        u32 umip_ok = 0;
        __asm__ volatile(
            "movl $7, %%eax; xorl %%ecx, %%ecx; cpuid;"
            "shrl $3, %%ecx; andl $1, %%ecx"
            : "=c"(umip_ok) : : "eax", "ebx", "edx");
        if (umip_ok) {
            u64 cr4v;
            __asm__ volatile("movq %%cr4, %0" : "=r"(cr4v));
            cr4v |= 0x800;   /* bit11 = UMIP */
            __asm__ volatile("movq %0, %%cr4" :: "r"(cr4v) : "memory");
        }
    }//防止Ring3--提权-->Ring0（仅当 CPU 支持 UMIP）
    { u64 cr0, cr3, cr4;
      __asm__ volatile("movq %%cr0, %0" : "=r"(cr0));
      __asm__ volatile("movq %%cr3, %0" : "=r"(cr3));
      __asm__ volatile("movq %%cr4, %0" : "=r"(cr4));
      char dbg[64]; ksprintf(dbg,"[KERNEL/INFO] CR0=%x CR3=%x CR4=%x",(u32)cr0,(u32)cr3,(u32)cr4); out_file_str(dbg); }
    /* 引导盘位置（引导器经 UEFI DevicePath 拿到的——不是猜的）：
       kind: 1=ATA/IDE 2=SATA(AHCI) 3=NVMe 4=USB / pci = bus<<16|dev<<8|func */
    { char dbg[96]; ksprintf(dbg,"[KERNEL/INFO] bootdev kind=%d pci=%x part_lba=%x part_sz=%x",
        info->ctrl_kind, info->pci_addr, info->part_lba, info->part_size); out_file_str(dbg); }
    /* KERN_BASE = runtime address of _start — compute into LOCAL first!
       (KERN_BASE itself lives in BSS — the zeroing loop below would wipe it!) */
    u64 kb;
    __asm__ volatile("lea _start(%%rip), %0" : "=r"(kb));
    /* 清零 BSS 段（未初始化全局变量区）——bootloader 只拷贝了 text+data，
       BSS 必须手动清零（C 全局变量初始值=0 靠这个）。用 RIP-relative lea 取地址 */
    {
        u64 bs, be;
        __asm__ volatile("lea %1, %0" : "=r"(bs) : "m"(_bss_start));
        __asm__ volatile("lea %1, %0" : "=r"(be) : "m"(_bss_end));
        for(u8 *p=(u8*)bs; p<(u8*)be; p++) *p=0;
    }
    KERN_BASE = kb;  /* AFTER zeroing */
    { char dbg[48];
        ksprintf(dbg,"[KERNEL/INFO] KB=%x",(u32)kb);
        out_file_str(dbg);
    }
    { char dbg[64]; ksprintf(dbg,"[KERNEL/INFO] build: %s", build_date); out_file_str(dbg); }
    { u64 bs2, be2;
        __asm__ volatile("lea %1, %0" : "=r"(bs2) : "m"(_bss_start));
        __asm__ volatile("lea %1, %0" : "=r"(be2) : "m"(_bss_end));
        char dbg[64];
        ksprintf(dbg,"[KERNEL/INFO] bss %x-%x sz=%x",(u32)bs2,(u32)be2,(u32)(be2-bs2)); LOG_INFO(dbg); }
    gdt_init();  // own GDT + TSS descriptor (was missing!)
    /* 关键：UEFI 的页表把低 4MB 标成"仅内核可访问"(US=0)——用户态(Ring3)
       一访问就 #PF。这里把 PML4E/PDPE/PDE 的 US 位(bit2)全置 1，
       让用户态能用低内存（用户栈/用户代码都在这里）。
       权限是四级页表 AND 出来的：任何一级 US=0 都不行，所以全改。 */
    {   u64 cr3v; __asm__ volatile("movq %%cr3, %0" : "=r"(cr3v));
        u64 *pml4t = (u64*)cr3v;
        u64 *pdptt = (u64*)(pml4t[0] & 0xFFFFF000);
        u64 *pdt = (u64*)(pdptt[0] & 0xFFFFF000);
        /* UEFI marks page-table pages read-only; CR0.WP=1 makes Ring0 writes
         to them #PF. Clear WP around the edits, then restore. */
        u64 cr0v; __asm__ volatile("movq %%cr0, %0" : "=r"(cr0v));
        __asm__ volatile("movq %0, %%cr0" :: "r"(cr0v & ~0x10000ULL) : "memory");
        { char dbg[64]; ksprintf(dbg,"[KERNEL/INFO] PTE %x %x %x",(u32)pml4t[0],(u32)pdptt[0],(u32)pdt[0]); out_file_str(dbg); }
        /* permission is ANDed across ALL levels: PML4E/PDPE were US=0 (0x23)! */
        pml4t[0] |= 0x4;  /* PML4E US=1 */
        pdptt[0] |= 0x4;  /* PDPTE US=1 */
        /* US 必须覆盖内核镜像 + .bss 占用的每一个 2MB 页：
           用户栈就在 .bss 里（jump_user 的 static 数组），写死 0..8MB
           的列表一旦有缓冲变大就失效——back_buf 涨到 1920x1080 后
           .bss 到 ~10.4MB、user_stack 落在 10.8MB，Ring3 第一条 push
           就 #PF（errcode 7 = P|W|U）。改成按 _bss_end 推导。 */
        u64 be_rt;
        __asm__ volatile("lea %1, %0" : "=r"(be_rt) : "m"(_bss_end));
        for (u64 i = 0; i <= (be_rt >> 21); i++)
            if (pdt[i] & 1) pdt[i] |= 0x4;   /* 只翻已存在的 PDE */
        __asm__ volatile("movq %0, %%cr0" :: "r"(cr0v) : "memory");
        /* flush TLB — stale US=0 entries would still #PF from Ring3!
         mov cr3 + invlpg for the low 4MB (QEMU big-page TLB is sticky) */
        __asm__ volatile("movq %0, %%cr3" :: "r"(cr3v) : "memory");
        for (u64 va = 0; va < 0x400000; va += 0x1000)
            __asm__ volatile("invlpg (%0)" :: "r"(va) : "memory");
        LOG_INFO("US bits set");
    }
    /* 设 LSTAR MSR（0xC0000082）= syscall 指令的入口——
       用户态执行 syscall 时 CPU 自动跳到这里（进内核） */
    {   u64 sce; __asm__ volatile("lea %1, %0" : "=r"(sce) : "m"(syscall_entry_asm));
        __asm__ volatile("movl $0xC0000082, %%ecx; movl %0, %%eax; movl %1, %%edx; wrmsr"
                         :: "r"((u32)sce), "r"((u32)(sce >> 32))
                         : "eax", "ecx", "edx", "memory");
        LOG_INFO("LSTAR set");
    }
    LOG_INFO("STARTUP gdt");
    u32 *fb = (u32*)info->fb_base;
    u32 hr=info->hr, vr=info->vr, st=info->stride;
    disp_init(fb, hr, vr, st);
    LOG_INFO("STARTUP disp");
    LOG_INFO("STARTUP pre");
    u128 range = 0;
    pid_t pid = pid_alloc();
    LOG_INFO("STARTUP pid");
    /* 图层管理器(shtctl)初始化：
       lm_map = 屏幕映射数组（每像素记"属于哪个图层"，供刷新用）
       shtctl = 图层管理器：管理所有图层（背景/窗口/光标）的叠放和刷新
       声明顺序重要：lm_map 在前、shtctl 在后，BSS 里不重叠 */
    static u8 lm_map[1920*1080];  // 屏幕映射数组（预留到 FHD）
    static SHTCTL shtctl; shtctl_init(&shtctl, fb, hr, vr, lm_map);
    LOG_INFO("STARTUP lmi");
    { char dbg[48]; ksprintf(dbg,"[KERNEL/INFO] fb=%x hr=%d vr=%d st=%d",(u32)(u64)fb,hr,vr,st); LOG_INFO(dbg); }

            /* 画桌面背景（直接写 framebuffer）：青绿色(0x008080)，
               底部 40px 是黑色任务栏 */
    for(u32 y=0;y<vr;y++){u32 c=(y>=vr-40)?0:0x00008080;
        for(u32 x=0;x<hr;x++)fb[y*st+x]=c;}
        LOG_INFO("STARTUP drawn");
        LOG_INFO("STARTUP desktop");
            put_str(fb,st,8,8,"WinDOS UEFI Kernel",0x00FFFFFF);
        put_str(fb,st,8,28,"Mouse: OK  Key: OK",0x00FFFFFF);
        LOG_INFO("STARTUP text");
            
    // ── 创建 "WinDOS" 演示窗口（带渐变阴影——合成：下层−depth×step）──
    static u32 win_buf[(200+2*SHADOW)*(120+2*SHADOW)];
    draw_win_shadow(win_buf, 200, 120, "WinDOS");
    LOG_INFO("STARTUP drawn");
    /* 背景图层（sht_back）——独立缓冲 back_buf（原版做法）：
       背景是最底图层（sid=0，map 里 0 就代表"桌面"）
       必须用独立内存缓冲而不是直接用 fb 本身（直接自拷贝慢）
       back_buf 内容 = 桌面青绿 + 底部任务栏黑色（和上面画的桌面一致，
       否则拖窗口经过任务栏会把任务栏擦掉） */
    /* 按最大模式(1920x1080)开——下面填充循环和 sheet_setbuf 都用 hr/vr，
       800x600 的数组按 1280x800 用会越界写 ~2.1MB，正好糊掉
       cur_buf / kstack / login_wbuf / g_fb / tss / user_stack */
    static u32 back_buf[1920*1080];
    /* background + taskbar (bottom 40px black) — must MATCH the desktop
       drawn by shtctl_refresh_all, or dragging a window over the taskbar
       erases it (back sheet repaints its buffer over it) */
    for (u32 y = 0; y < vr; y++) {
        u32 bg = (y >= vr-40) ? 0 : 0x00008080;
        for (u32 x = 0; x < hr; x++) back_buf[y*hr+x] = bg;
    }
    struct SHEET *sht_back = sheet_alloc(&shtctl);
    sheet_setbuf(sht_back, back_buf, hr, vr, (u32)-1);
    sht_back->vx0 = 0; sht_back->vy0 = 0;
    sheet_updown(sht_back, 0);
    struct SHEET *win_sht = sheet_alloc(&shtctl);
    sheet_setbuf(win_sht, win_buf, 200 + 2 * SHADOW, 120 + 2 * SHADOW, COL_INV);
    win_sht->vx0 = 80 - SHADOW; win_sht->vy0 = 72 - SHADOW;
    LOG_INFO("STARTUP wadd");
    sheet_updown(win_sht, 1);
    g_shadow_shts[g_shadow_cnt++] = win_sht;   /* ★ 注册阴影窗口——刷新联动 */
    LOG_INFO("STARTUP win");

    // ── 创建鼠标光标图层（18x16 箭头）──
    // cur_buf 先全填透明色(0x00FF00FF)，再把 cs 字符画写进去
    // （b=黑色 o=白色，/ 保持透明）
    static u8 cur_buf[18*16*4];
    for(int i=0;i<18*16*4;i++){cur_buf[i]=0xFF;((u32*)cur_buf)[i/4]=0x00FF00FF;}
    for(u32 dy=0;dy<16;dy++)for(u32 dx=0;dx<18;dx++){
        u8 p=cs[dy][dx];if(p=='/')continue;
        ((u32*)cur_buf)[dy*18+dx]=(p=='b')?0x00000000:0x00FFFFFF;   /* 黑/白都高位 0x00——0xFF 高位会与阴影半透明标记冲突 */
    }
    struct SHEET *cur_sht = sheet_alloc(&shtctl);
    sheet_setbuf(cur_sht, (u32*)cur_buf, 18, 16, COL_INV);
    LOG_INFO("STARTUP cursor");
    
    int mx=hr/2, my=vr/2;
    cur_sht->vx0=mx; cur_sht->vy0=my;
    sheet_updown(cur_sht, 2);
    g_shtctl = &shtctl; g_cur_sht = cur_sht; g_mx = mx; g_my = my;
    /* ── 中断系统初始化 ──
       默认所有中断向量 → isr_default；异常 0-31 → 死屏(BSOD)
       0x20=PIT 时钟  0x21=键盘  0x2c=鼠标
       然后 PIC 初始化、PIT 定时器、键盘/鼠标驱动、开中断(sti) */
    /* ★ 内核栈(TSS.RSP0)：中断/系统调用切换的栈——必须在开中断(sti)前设置，
       否则中断触发→CPU 切到未设置的 RSP0(垃圾栈)→中断处理卡死(freeze) */
    static u8 kstack[16384];
    u64 ksp;
    __asm__ volatile("lea %1, %0" : "=r"(ksp) : "m"(kstack[0]));
    tss_set_rsp0(ksp + sizeof(kstack));

    for(int i=0;i<256;i++)set_gate(i,(void*)isr_default);
    exc_init();   // 异常 0-31 → 蓝屏
    set_gate(0x20,(void*)isr20);
    set_gate(0x21,(void*)isr21);
    set_gate(0x2c,(void*)isr2c);
    { u64 g = ((u64)idt[0x20].hi<<32)|((u64)idt[0x20].mid<<16)|idt[0x20].lo; char dbg[48]; ksprintf(dbg,"[int] gate20=%x",(u32)g); LOG_INFO(dbg); }
    lidt_idt(); LOG_INFO("[int] lidt"); pic_init(); LOG_INFO("[int] pic"); pit_init(); LOG_INFO("[int] pit"); init_keyboard(); LOG_INFO("[int] kbd"); mouse_enable(); LOG_INFO("[int] mouse"); __asm__ volatile("sti"); LOG_INFO("[int] sti");
    LOG_INFO("STARTUP idt");
    
    /* 桌面刷新 + 登录窗口创建期间保持中断关闭：鼠标中断 isr2c 也在动图层表
       （sheet_free/shtctl_refresh_all/sheet_updown），这些操作不可重入——
       鼠标包正好落在刷新中间会踩坏图层表（野指针写 → #PF）。
       IF 由主循环的 io_stihlt() 恢复。 */
    io_cli();
    LOG_INFO("[login]11 refresh");
    shtctl_refresh_all(&shtctl);
    LOG_INFO("STARTUP refresh");
    MP m={};int errors=0;
    int key_shift=0, key_leds=0, bsod_triggered=0;
    struct SHEET *sht=nullptr; int mmx=-1, mmy=-1; int x,y;
    // ── 登录窗口（Ring 0 界面，不可关闭——保证系统入口永远存在）──
    // login_buf = 用户名输入缓冲（最长 32）
    // login_wbuf = 登录窗口的像素缓冲（400x200）
    // 输入 "sysdebug" 按回车 → Ring 1；其他名字 → Ring 3
    LOG_INFO("[login]1 buf");
    static char login_buf[32]; int login_len = 0; int logged_in = 0;
    LOG_INFO("[login]2 wbuf");
    static u32 login_wbuf[(400+2*SHADOW)*(200+2*SHADOW)];   /* ★ 带阴影（加大缓冲） */
    LOG_INFO("[login]3 draw_shadow");
    draw_win_shadow(login_wbuf, 400, 200, "WinDOS Login");   /* ★ 阴影窗口 */
    LOG_INFO("[login]4 put_str");
    put_str(login_wbuf, 400+2*SHADOW, 20+SHADOW, 40+SHADOW, "Username:", 0x00CCCCCC);
    for(int y=0;y<16;y++)for(int x=0;x<240;x++) login_wbuf[(40+SHADOW+y)*(400+2*SHADOW)+(160+SHADOW+x)]=0x00101010;
    LOG_INFO("[login]5 alloc");
    struct SHEET *login_sht = sheet_alloc(&shtctl);
    LOG_INFO("[login]6 setbuf");
    sheet_setbuf(login_sht, login_wbuf, 400 + 2 * SHADOW, 200 + 2 * SHADOW, COL_INV);
    LOG_INFO("[login]7 pos");
    login_sht->vx0=hr/2-200-SHADOW; login_sht->vy0=vr/2-100-SHADOW;
    LOG_INFO("[login]8 updown");
    sheet_updown(login_sht, 3);
    LOG_INFO("[login]9 shadow");
    g_shadow_shts[g_shadow_cnt++] = login_sht;   /* ★ 登录窗口也注册为阴影窗口 */
    LOG_INFO("[login]10 cur");
    sheet_updown(cur_sht, shtctl.top);   /* ★ 光标最顶（top 内——不超界——后 updown 的在上） */
    shtctl_refresh_all(&shtctl);   /* ★ 登录窗口创建后必须全屏刷新——否则启动时
                                      显示异常（map 没更新——部分黑）——拖动才恢复 */
    LOG_INFO("[login]12 glogin");
    g_login_sht = login_sht;
    LOG_INFO("STARTUP login");
    /* ── ExFAT 测试：初始化 + 列根目录（fs/exfat.cpp——QEMU 第二块盘）── */
    LOG_INFO("[FS] exfat-test enter");
    {
        /* ATA 诊断：Secondary 通道状态（0x177）——0xFF=无盘，0x50/0x58=盘就绪 */
        char adbg[64];
        u8 ast = in8(0x177);
        ksprintf(adbg, "[FS] ATA Sec st=%x\n", ast);
        out_file_str(adbg);
        u8 ast2 = in8(0x1F7);
        ksprintf(adbg, "[FS] ATA Pri st=%x\n", ast2);
        out_file_str(adbg);
        /* 错误寄存器（0x171 Secondary / 0x1F1 Primary）：ABRT=bit2 IDNF=bit4 */
        u8 aerr = in8(0x171);
        ksprintf(adbg, "[FS] ATA Sec err=%x\n", aerr);
        out_file_str(adbg);
        /* 手动 ATA 读 Primary LBA 0（master 0xE0 和 slave 0xF0——诊断） */
        {
            out8(0x3F6, 0x02);   /* SRST：软件复位 */
            for (volatile int d2 = 0; d2 < 100000; d2++);
            out8(0x3F6, 0x00);   /* 复位完成（nIEN 清） */
            for (volatile int d2 = 0; d2 < 100000; d2++);
            LOG_INFO("[FS] ATA manual-read");
            for (int dv = 0; dv <= 2; dv++) {   /* 0=master读 1=slave读 2=slave IDENTIFY */
                int sel = (dv == 2) ? 1 : dv;
                out8(0x1F6, sel ? 0xF0 : 0xE0);   /* 选盘 */
                for (volatile int d2 = 0; d2 < 10000; d2++);   /* 选盘后延时 */
                out8(0x1F2, 1);
                out8(0x1F3, 0); out8(0x1F4, 0); out8(0x1F5, 0);
                u8 cmd = (dv == 2) ? 0xEC : 0x20;   /* dv=2 → IDENTIFY */
                out8(0x1F7, cmd);
                int got = 0;
                u8 st3 = 0;
                for (volatile int dly = 0; dly < 2000; dly++) {   /* 忙等减到 2k 次——QEMU IDE 无盘 DRQ 不置位——原 5M 次 volatile 端口读卡死(freeze) */
                    st3 = in8(0x1F7);
                    if (st3 & 0x08) { got = 1; break; }   /* DRQ */
                    if (st3 & 0x01) break;                /* 错误 */
                }
                u8 er3 = in8(0x1F1);
                u16 first = (st3 & 0x08) ? (u16)(in8(0x1F0) | (in8(0x1F0) << 8)) : 0;
                ksprintf(adbg, "[FS] P dv=%d cmd=%x got=%d st=%x err=%x data=%x\n", dv, cmd, got, st3, er3, first);
                out_file_str(adbg);
            }
        }
        LOG_INFO("[FS] exfat_init call");
        int r = exfat_init();
        LOG_INFO("[FS] exfat_init done");
        /* ── QEMU 检测：CPUID hypervisor leaf 0x40000000
           QEMU TCG → "TCGTCGTCG"、KVM → "KVMKVMKVM"（真硬件无 hypervisor） ── */
        {
            u32 a, b, c_, d_;
            __asm__ volatile("cpuid" : "=a"(a), "=b"(b), "=c"(c_), "=d"(d_) : "a"(0x40000000));
            char hv[13];
            *(u32*)(hv + 0) = b; *(u32*)(hv + 4) = c_; *(u32*)(hv + 8) = d_;
            hv[12] = 0;
            int q = (hv[0]=='T'&&hv[1]=='C'&&hv[2]=='G') ||   /* TCGTCGTCG */
                    (hv[0]=='K'&&hv[1]=='V'&&hv[2]=='M');    /* KVMKVMKVM */
            char ndbg[64];
            ksprintf(ndbg, "[CPU] hypervisor=\"%s\" qemu=%d\n", hv, q);
            out_file_str(ndbg);
            /* ① IDE 特殊处理：QEMU 的 IDE PIO 模拟有坑（DRQ 永不置位——
               十几轮诊断确认）——标注环境，后续走 NVMe */
            if (q) {
                out_file_str("[FS] QEMU env: IDE PIO known-broken, using NVMe\n");
            }
        }
        /* ── NVMe 探测：PCI 枚举打印（bus 0 所有 dev 的 vid/class——诊断）── */
        {
            char ndbg[64];
            for (u8 dv = 0; dv < 8; dv++) {
                u32 vid = pci_read(0, dv, 0, 0) & 0xFFFF;
                u32 cls = pci_read(0, dv, 0, 8);
                ksprintf(ndbg, "[NVMe] bus0 dev%d vid=%x cls=%x/%x\n",
                         dv, vid, (cls >> 24) & 0xFF, (cls >> 16) & 0xFF);
                out_file_str(ndbg);
            }
            /* dev4（vid 1B36——QEMU nvme）的 BAR0/BAR1 原始值 */
            {
                u32 b0 = pci_read(0, 4, 0, 0x10);
                u32 b1 = pci_read(0, 4, 0, 0x14);
                ksprintf(ndbg, "[NVMe] dev4 bar0=%x bar1=%x\n", b0, b1);
                out_file_str(ndbg);
            }
            u64 bar = nvme_find();
            ksprintf(ndbg, "[NVMe] find=%x\n", (u32)bar);
            out_file_str(ndbg);
        }
        if (r == 0) {
            out_file_str("[FS] exFAT init OK\n");
            struct EXFAT_FILE_INFO infos[32];
            int n = exfat_list_dir(2, infos, 32);   /* 根目录簇 = 2 */
            char dbg[80];
            ksprintf(dbg, "[FS] root entries: %d\n", n);
            out_file_str(dbg);
            for (int i = 0; i < n && i < 32; i++) {
                ksprintf(dbg, "[FS]   %s  size=%d %s\n",
                         infos[i].name, (u32)infos[i].data_len,
                         infos[i].is_dir ? "[DIR]" : "");
                out_file_str(dbg);
            }
            /* ── 验证文件数据读取：读第一个文件内容打印 ── */
            if (n > 0) {
                u8 fbuf[64];
                if (exfat_read_file(infos[0].first_cluster,
                                    infos[0].data_len, fbuf) == 0) {
                    fbuf[infos[0].data_len] = 0;
                    ksprintf(dbg, "[FS] content: %s\n", fbuf);
                    out_file_str(dbg);
                } else {
                    out_file_str("[FS] read FAIL\n");
                }
            }
            /* ── 写文件测试：创建 TEST.TXT → 读回验证 ── */
            {
                const char *wdata = "HI FROM WINDOS";
                int wr = exfat_write_file("TEST.TXT", (const u8*)wdata, 14);
                if (wr == 0) {
                    out_file_str("[FS] write OK\n");
                    struct EXFAT_FILE_INFO wfi = {};   /* 初始化——未初始化记录类型（Clang-Tidy 警告）潜在崩溃 */
                    if (exfat_find(2, "TEST.TXT", &wfi) == 0) {
                        u8 rbuf[64];
                        if (exfat_read_file(wfi.first_cluster, wfi.data_len, rbuf) == 0) {
                            rbuf[wfi.data_len] = 0;
                            ksprintf(dbg, "[FS] readback: %s\n", rbuf);
                            out_file_str(dbg);
                        }
                    }
                } else {
                    ksprintf(dbg, "[FS] write FAIL r=%d\n", wr);
                    out_file_str(dbg);
                }
            }
        } else {
            char dbg[48]; ksprintf(dbg, "[FS] exFAT init FAIL r=%d\n", r);
            out_file_str(dbg);
        }
    }
    /* ═══════════════ 主循环（登录界面阶段）═══════════════
       事件驱动：无输入就 io_stihlt（开中断+停机——省电，被中断唤醒）
       有键盘输入 → 处理（登录输入/Shift/CapsLock/Ctrl+Shift+B）
       鼠标 → 不在主循环了！isr2c 中断里直接处理（见 sys/isr.h）
       每 200 个时钟 tick 打印一次心跳日志（证明系统活着） */
    LOG_INFO("[FS] mainloop enter");
    volatile u32 last_tick = 0; int hb = 0;
    u32 t0_sec = (u32)info->tm_hour * 3600 + (u32)info->tm_min * 60 + (u32)info->tm_sec;   /* GetTime 初始时间转秒 */
    LOG_INFO("Main 'for (;;)'entering.");
    for (int n = 0; true; n++) {
        io_cli();
        /* ORIGINAL: NO per-frame sheet_updown — cursor top is handled in the
           click handler only (sheet_updown(cur_sht, top) there). Per-frame
           reorder interleaves with sheet_slide refreshes → crash. */
        if(g_ticks != last_tick){
            last_tick = g_ticks;
            hb++;
            if(hb % 200 == 0){ char dbg[48]; ksprintf(dbg,"[KERNEL/INFO] t=%d i21=%d SYSTEM ALIVE",g_ticks,_isr21_fires); LOG_INFO(dbg); }
            if(g_ticks % 100 == 0){
                /* 任务栏时钟（GetTime 初始时间 + PIT 累加——走字） */
                u32 now = t0_sec + g_ticks / 100;
                u8 rh = (u8)(now / 3600 % 24);
                u8 rm = (u8)(now / 60 % 60);
                char tbuf[8];
                tbuf[0] = '0' + rh / 10; tbuf[1] = '0' + rh % 10;
                tbuf[2] = ':';
                tbuf[3] = '0' + rm / 10; tbuf[4] = '0' + rm % 10;
                tbuf[5] = 0;
                for (int yy = 0; yy < 20; yy++) for (int xx = 0; xx < 56; xx++)
                    back_buf[(vr - 20 + yy) * hr + (hr - 56 + xx)] = 0x00000000;
                put_str(back_buf, hr, hr - 52, vr - 14, tbuf, 0x00FFFFFF);
                sheet_refresh(sht_back, hr - 56, vr - 20, hr, vr);
            }
        }
        u8 d;  // 键盘扫描码
        if(fg(&kfifo,&d)){   // 从键盘 FIFO 取一个扫描码（中断塞进来的）
            io_sti();
            {char dbg[32]; ksprintf(dbg,"[KERNEL/INFO] k=%02X",d); LOG_INFO(dbg);}
            if(d<0x80){   // 按下键（<0x80）；>=0x80 是释放
                // 扫描码 → 字符（Shift 按下用 keytable1，否则 keytable0）
                char s0 = (key_shift==0) ? keytable0[d] : keytable1[d];
                // 大小写规则（原版）：大写字母在
                //   CapsLock 关 + 无 Shift  → 转小写
                //   CapsLock 开 + 有 Shift  → 转小写
                // （即 CapsLock 与 Shift 互反）
                if('A'<=s0&&s0<='Z'){
                    if(((key_leds&4)==0&&key_shift==0)||((key_leds&4)!=0&&key_shift!=0))
                        s0+=0x20;
                }
                if(!logged_in){   // 还没登录 → 处理登录输入
                    if(s0==0x0a){ /* 回车：检查用户名 */
                        LOG_INFO("[login] enter");
                        login_buf[login_len]=0;
                        out_file_str("\n[LOGIN] user='");
                        out_str(login_buf);
                        out_file_str("'\n");
                        /* Sysdebug → Ring 1, everyone else → Ring 3 */
                        int ring = (login_buf[0]=='S'&&login_buf[1]=='y'&&login_buf[2]=='s'
                                   &&login_buf[3]=='d'&&login_buf[4]=='e'&&login_buf[5]=='b'
                                   &&login_buf[6]=='u'&&login_buf[7]=='g'&&login_buf[8]==0) ? 1 : 3;
                        {char dbg[32]; ksprintf(dbg,"[KERNEL/TO USER/LOGIN] ring=%d",ring);
                            out_file_str(dbg);
                            out_file_str("\n");}
                        logged_in=1;
                        LOG_INFO("[login] ok");
                        /* 登录完成——销毁登录窗口（不让它留在桌面上）
                           然后全屏刷新一次（画掉窗口残留）。
                           整个拆除→sysret 窗口内关中断：否则 isr2c 会在
                           sheet_free/shtctl_refresh_all 中间重入图层表。
                           sysret 从 R11(0x202) 恢复 IF=1——用户态照常有中断。 */
                        io_cli();
                        sheet_free(login_sht);
                        LOG_INFO("[login] freed");
                        shtctl_refresh_all(&shtctl);
                        LOG_INFO("[login] refreshed");
                        {char dbg[32]; ksprintf(dbg,"J%d ",ring); out_str(dbg);}
                        LOG_INFO("[login] jump_user");
                        jump_user(user_main, ring);   // ← 跳进用户态（sysret）
                    } else if(s0==0x08){ /* Backspace */
                        if(login_len>0) login_len--;
                    } else if(s0>=' '&&login_len<31){
                        login_buf[login_len++]=s0;
                    }
                    /* 重画登录窗口里的输入框：先清空区域(黑)，再写当前输入内容 */
                    for(int y=0;y<16;y++)for(int x=0;x<240;x++) login_wbuf[(40+y)*400+(160+x)]=0x00101010;
                    char line[40];
                    for(int i=0;i<login_len&&i<31;i++) line[i]=login_buf[i];
                    line[login_len>31?31:login_len]=0;
                    put_str(login_wbuf, 400 + 2 * SHADOW, 160 + SHADOW, 40 + SHADOW, line, 0x00FFFFFF);
                    io_cli();  /* 图层操作——不能被 isr2c 打断 */
                    sheet_refresh(login_sht, 150 + SHADOW, 32 + SHADOW, 410 + SHADOW, 64 + SHADOW);   /* 内容偏移+SHADOW */
                    io_sti();
                }
            }
            if(d==0x2a) key_shift|=1;
            if(d==0x36) key_shift|=2;
            if(d==0xaa) key_shift&=~1;
            if(d==0xb6) key_shift&=~2;
            if(d==0x3a){   // CapsLock：切换 LED 状态，并给键盘发 LED 命令(0xED)
                key_leds^=4;
                for(int j=0;j<0x2000;j++){if((in8(0x64)&2)==0)break;}  // 等键盘控制器就绪
                out8(0x60,0xED);
                for(int j=0;j<0x2000;j++){if((in8(0x64)&2)==0)break;}
                out8(0x60,key_leds);
            }
            if(d==0xFF){ /* PS/2 error/resend — keyboard state unknown; clear
                           ghost keys so random bytes can't fake Ctrl+Shift+B */
                for(int i=0;i<256;i++) keystatus[i]=0;
            } else if(d<0x80) keystatus[d]=1;
            else keystatus[d&0x7F]=0;
            /* 调试热键：Ctrl+Shift+B → 故意蓝屏（死屏）
               用真实的 Shift 状态(key_shift)判断——keystatus 可能被 0xFF
               错误字节污染（幽灵键），之前误触发过，所以双保险 */
            if(!bsod_triggered&&keystatus[0x1D]&&(key_shift!=0)&&keystatus[0x30]){
                bsod_triggered=1;
                LOG_CRITICAL("User triggered panic (Ctrl+Shift+B)");
                panic("USER_CTRL_SHIFT_B");
            }
        } else {
            io_stihlt();  /* 无输入 → 开中断+停机（睡眠，被中断唤醒）
                           鼠标全在 isr2c 中断里处理（复制自原来这里的代码），
                           所以登录界面和用户态下窗口都能拖 */
        }
    }
}
// C++ 运行库 stub（裸机没有标准库，链接器需要这些符号）
void operator delete(void*,unsigned long){}
extern "C" void __cxa_atexit(){}
extern "C" void *memset(void*d,int v,unsigned long n){unsigned char*p=(unsigned char*)d;while(n--)*p++=(unsigned char)v;return d;}
