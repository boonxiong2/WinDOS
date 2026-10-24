/* fs/nvme.h — NVMe 驱动（QEMU 模拟——PCIe NVMe SSD）
   流程：PCI 枚举(0xCF8/0xCFC) → 找 NVMe → BAR0(MMIO 寄存器)
        → 初始化(CC/AQA/队列) → IDENTIFY → IO 队列读(PRP)
   当前：PCI 枚举 + BAR0 拼接 + 控制器初始化(Admin 队列) + IDENTIFY 控制器 */
#pragma once
#include "../boot/types.h"
#include "../drivers/io.h"
#include "../desk/page.h"

/* ---- 32 位端口 I/O（io.h 只有 8 位——补） ---- */
static inline u32 inl(u16 p) { u32 v; __asm__ volatile("inl %1,%0":"=a"(v):"dN"(p):"memory"); return v; }
static inline void outl(u16 p, u32 v) { __asm__ volatile("outl %0,%1"::"a"(v),"dN"(p):"memory"); }

/* ---- PCI 配置空间访问 ---- */
static inline u32 pci_read(u8 bus, u8 dev, u8 fn, u8 reg) {
    outl(0xCF8, 0x80000000u | ((u32)bus<<16) | ((u32)dev<<11) | ((u32)fn<<8) | (reg & 0xFC));
    return inl(0xCFC);
}
static inline void pci_write(u8 bus, u8 dev, u8 fn, u8 reg, u32 val) {
    outl(0xCF8, 0x80000000u | ((u32)bus<<16) | ((u32)dev<<11) | ((u32)fn<<8) | (reg & 0xFC));
    outl(0xCFC, val);
}

/* ---- NVMe MMIO 寄存器（BAR0 偏移） ---- */
#define NVME_CAP   0x00   /* 容量：bit0-15=MPSMIN bit16-31=MPSMAX bit32-47=TO bit48-51=CSS */
#define NVME_CC    0x14   /* 控制器配置：bit0=EN | bit6:4=CSS | bit10:7=MPS | bit19:16=IOSQES | bit23:20=IOCQES */
                          /* ★ 注意：IOSQES/IOCQES 在 bits19:16 / bits23:20，不是 bit8-11/bit4-7
                             （放错的后果：控制器按 2^0=1 字节读 SQE → 命令永远收不全 → IDENTIFY 超时 -2）*/
#define NVME_CSTS  0x1C   /* 状态：bit0=RDY */
#define NVME_AQA   0x24   /* Admin 队列属性：bit0-11=ASQS bit16-27=ACQS */
#define NVME_ASQ   0x28   /* Admin 提交队列基址（物理，页对齐） */
#define NVME_ACQ   0x30   /* Admin 完成队列基址 */

static inline u32 nvme_read32(u64 bar, u32 off) { return *(volatile u32*)(u64)(bar + off); }
static inline void nvme_write32(u64 bar, u32 off, u32 v) { *(volatile u32*)(u64)(bar + off) = v; }
/* 64 位 MMIO 寄存器必须分两次 32 位写（低在前，高在后） */
static inline void nvme_write64(u64 bar, u32 off, u64 v) {
    nvme_write32(bar, off,     (u32)v);
    nvme_write32(bar, off + 4, (u32)(v >> 32));
}

/* ---- PCI 枚举：找 NVMe（Class 0x01/0x08 或 vendor/device）——返回 BAR0 或 0 ---- */
static inline u64 nvme_find(void) {
    for (u8 bus = 0; bus < 4; bus++) {
        for (u8 dev = 0; dev < 32; dev++) {
            u32 vid = pci_read(bus, dev, 0, 0) & 0xFFFF;
            if (vid == 0xFFFF || vid == 0) continue;   /* 无设备 */
            u32 cls = pci_read(bus, dev, 0, 8);        /* Class/Subclass（偏移 8） */
            u8 base_cls = (cls >> 24) & 0xFF;          /* Class */
            u8 sub_cls  = (cls >> 16) & 0xFF;          /* Subclass */
            /* ★ QEMU 的 nvme（-device nvme）vid=0x1B36（Red Hat）且 Class 编码不规范
               （实测 cls=00/10 而非标准 01/08）——用 vid 匹配兜底 */
            if ((base_cls == 0x01 && sub_cls == 0x08) || (vid == 0x1B36)) {
                u32 bar0 = pci_read(bus, dev, 0, 0x10);
                u32 bar1 = pci_read(bus, dev, 0, 0x14);
                u64 bar = (u64)(bar0 & 0xFFFFFFF0);
                /* ★ 64 位 BAR：类型位(bit2-1==10) 或 vid 1B36（QEMU nvme 是
                   64 位 BAR——低 32 位可能全 0——必须读 BAR1 高 32 位） */
                if ((bar0 & 0x6) == 0x4 || vid == 0x1B36) {
                    bar |= ((u64)bar1 << 32);
                }
                return bar;
            }
        }
    }
    return 0;   /* 没找到 */
}

/* ================= 控制器初始化（Admin 队列）=================
   Admin SQ: 64 项 × 64B = 4096B（1 页）
   Admin CQ: 64 项 × 16B = 1024B（1 页够）
   门铃基址 = BAR0 + 0x1000（SQ0 tail @0x1000, CQ0 head @0x1004） */
#define NVME_ADMIN_QD   63                       /* 队列深度-1（64 项） */
#define NVME_SQ_ENTRY   64                       /* SQE 64 字节 */
#define NVME_CQ_ENTRY   16                       /* CQE 16 字节 */

typedef struct {
    u64 bar;
    volatile u64 *asq;       /* Admin SQ（64 项 × 8 个 u64 = 512 u64） */
    volatile u64 *acq;
    u32 sq_tail;
    u32 cq_head;
    u32 cq_phase;            /* ★ 初始 1（不是 0）—— QEMU/CQE 相位语义 */
    u32 cid;
    int ready;
} NVMeDev;

extern NVMeDev g_nvme;   /* 唯一定义在 kernel.cpp —— 不能放 static，否则每个 .cpp 各一份副本 */

/* 提交一条 Admin 命令（dw = 16 个 u32 的 SQE），同步轮询收 CQE
   返回 0 成功；>0 = NVMe status；-1 参数/未就绪；-2 超时 */
static int nvme_admin_cmd(u32 *dw, u32 *out_dw0) {
    if (!g_nvme.ready) return -1;

    /* CID 写在 SQE 的 dw0 bit16-31 */
    if (g_nvme.cid == 0) g_nvme.cid = 1;
    dw[0] = (dw[0] & 0xFFFFu) | (g_nvme.cid << 16);
    u32 cid = g_nvme.cid++;

    /* 写 SQE（64B）到 SQ 的 tail 位置 */
    volatile u64 *sq = g_nvme.asq;
    u32 pos = g_nvme.sq_tail;
    for (u32 i = 0; i < 8; i++) {
        sq[pos * 8 + i] = ((u64)dw[i * 2]) | ((u64)dw[i * 2 + 1] << 32);
    }

    /* 推进 tail + 内存屏障（SQ 内容必须先于门铃可见）+ 敲 SQ 门铃 */
    g_nvme.sq_tail = (pos + 1) % (NVME_ADMIN_QD + 1);
    __asm__ volatile("" ::: "memory");
    nvme_write32(g_nvme.bar, 0x1000, g_nvme.sq_tail);

    /* 轮询 CQE：相位位在 dword3 的 bit16（★ 不是 bit1） */
    for (u32 spin = 0; spin < 20000000u; spin++) {
        volatile u32 *cq = (volatile u32*)g_nvme.acq;
        u32 dw3 = cq[g_nvme.cq_head * 4 + 3];
        if (((dw3 >> 16) & 1u) == g_nvme.cq_phase) {
            u32 sc  = (dw3 >> 17) & 0xFFu;
            u32 sct = (dw3 >> 25) & 0x7u;
            if (out_dw0) *out_dw0 = cq[g_nvme.cq_head * 4 + 0];
            /* 推进 head；绕回 0 时相位翻转 */
            g_nvme.cq_head = (g_nvme.cq_head + 1) % (NVME_ADMIN_QD + 1);
            if (g_nvme.cq_head == 0) g_nvme.cq_phase ^= 1u;
            __asm__ volatile("" ::: "memory");
            /* 敲 CQ 门铃（head）@0x1004 */
            nvme_write32(g_nvme.bar, 0x1004, g_nvme.cq_head);
            if (sc || sct) return (int)(sc | (sct << 8));   /* 命令失败 */
            return 0;
        }
        __asm__ volatile("pause");
    }
    return -2;   /* 超时 */
}

/* IDENTIFY（CNS=1 控制器 / CNS=2 命名空间列表），数据 4096B 写到 buf
   返回 0 成功 */
static int nvme_identify(u32 cns, u32 nsid, void *buf) {
    if (!g_nvme.ready) return -1;
    u32 dw[16];
    for (int i = 0; i < 16; i++) dw[i] = 0;
    dw[0]  = 0x06;                              /* opcode = IDENTIFY */
    dw[1]  = nsid;                              /* NSID（CNS=1 时可 0） */
    /* dw[6..9] = PRP1（64 位物理地址）—— 数据缓冲 */
    u64 prp = (u64)buf;
    dw[6]  = (u32)prp;
    dw[7]  = (u32)(prp >> 32);
    dw[10] = cns;                               /* CNS: 1=controller, 2=ns list */
    return nvme_admin_cmd(dw, 0);
}

/* ================= 第 8 步：I/O 队列 + 读写 =================
   I/O SQ/CQ (qid=1)：各 64 项，物理连续（page_alloc 保证 4KB 对齐）
   门铃偏移通式：SQ(qid) tail = 0x1000 + (2*qid)   *4
                 CQ(qid) head = 0x1000 + (2*qid+1) *4
   所以 qid=0 → 0x1000 / 0x1004（Admin）；qid=1 → 0x1008 / 0x100C */
#define NVME_IO_QD        63                          /* 队列项数-1（64 项） */
#define NVME_DB_SQ(qid)   (0x1000u + ((u32)(qid) * 2u) * 4u)
#define NVME_DB_CQ(qid)   (0x1000u + ((u32)(qid) * 2u + 1u) * 4u)

extern volatile u64 *g_iosq;      /* 同 g_nvme：定义在 kernel.cpp，所有 .cpp 共用 */
extern volatile u64 *g_iocq;
extern u32 g_iosq_tail, g_iocq_head, g_iocq_phase;

/* 向 I/O 队列(qid=1)提交一条命令并同步等完成
   返回 0 成功；>0 = NVMe status；-1 未就绪；-2 超时 */
static int nvme_io_cmd(u32 *dw, u32 *out_dw0) {
    if (!g_nvme.ready || !g_iosq || !g_iocq) return -1;

    if (g_nvme.cid == 0) g_nvme.cid = 1;
    dw[0] = (dw[0] & 0xFFFFu) | (g_nvme.cid << 16);
    g_nvme.cid++;

    u32 pos = g_iosq_tail;
    for (u32 i = 0; i < 8; i++)
        g_iosq[pos * 8 + i] = ((u64)dw[i * 2]) | ((u64)dw[i * 2 + 1] << 32);

    g_iosq_tail = (pos + 1) % (NVME_IO_QD + 1);
    __asm__ volatile("" ::: "memory");                /* SQ 内容先于门铃可见 */
    nvme_write32(g_nvme.bar, NVME_DB_SQ(1), g_iosq_tail);

    for (u32 spin = 0; spin < 20000000u; spin++) {
        u32 dw3 = ((volatile u32*)g_iocq)[g_iocq_head * 4 + 3];
        if (((dw3 >> 16) & 1u) == g_iocq_phase) {     /* 相位位 = dword3 bit16 */
            u32 sc  = (dw3 >> 17) & 0xFFu;
            u32 sct = (dw3 >> 25) & 0x7u;
            if (out_dw0) *out_dw0 = ((volatile u32*)g_iocq)[g_iocq_head * 4 + 0];
            g_iocq_head = (g_iocq_head + 1) % (NVME_IO_QD + 1);
            if (g_iocq_head == 0) g_iocq_phase ^= 1u;
            __asm__ volatile("" ::: "memory");
            nvme_write32(g_nvme.bar, NVME_DB_CQ(1), g_iocq_head);
            if (sc || sct) return (int)(sc | (sct << 8));
            return 0;
        }
        __asm__ volatile("pause");
    }
    return -2;
}

/* 建 I/O CQ + SQ（qid=1；PC=1 物理连续、IEN=0 不用中断）
   返回 0 成功；-1 分配失败；其它 = 负的 Admin 错误码（-10-r / -20-r） */
static int nvme_create_io_queues(void) {
    u64 *cq = (u64*)page_alloc(1);
    u64 *sq = (u64*)page_alloc(1);
    if (!cq || !sq) return -1;
    for (u32 i = 0; i < 512; i++) { cq[i] = 0; sq[i] = 0; }

    g_iocq = (volatile u64*)cq;
    g_iosq = (volatile u64*)sq;
    g_iosq_tail = 0; g_iocq_head = 0; g_iocq_phase = 1;
    u32 dw[16];
    int r;

    /* Create I/O Completion Queue — opcode 0x05
       dw10: QID(15:0) | QSIZE(31:16)；dw11: PC(1:0) | IEN(2) | IV(31:16) */
    for (int i = 0; i < 16; i++) dw[i] = 0;
    dw[0]  = 0x05;
    dw[6]  = (u32)(u64)cq;
    dw[7]  = (u32)((u64)cq >> 32);
    dw[10] = 1u | ((u32)NVME_IO_QD << 16);
    dw[11] = 1u;                                      /* PC=1（物理连续） */
    r = nvme_admin_cmd(dw, 0);
    if (r) return -10 - r;

    /* Create I/O Submission Queue — opcode 0x01
       dw10: QID | QSIZE；dw11: PC(1:0) | QPRIO(3:2) | CQID(31:16) */
    for (int i = 0; i < 16; i++) dw[i] = 0;
    dw[0]  = 0x01;
    dw[6]  = (u32)(u64)sq;
    dw[7]  = (u32)((u64)sq >> 32);
    dw[10] = 1u | ((u32)NVME_IO_QD << 16);
    dw[11] = (1u << 16) | 1u;                         /* CQID=1, PC=1 */
    r = nvme_admin_cmd(dw, 0);
    if (r) return -20 - r;
    return 0;
}

/* 读/写：opcode 0x02=READ / 0x01=WRITE
   slba = 起始 LBA（64 位），nlb = 块数（>=1，每块 512B），buf 用 page_alloc（4KB 对齐）
   一块 = 512B；一页 = 4KB = 8 块（PRP1 单页够用） */
static int nvme_rw(u32 opcode, u32 nsid, u64 slba, u32 nlb, void *buf) {
    u32 dw[16];
    for (int i = 0; i < 16; i++) dw[i] = 0;
    dw[0]  = opcode;
    dw[1]  = nsid;                                    /* NSID=1（命名空间 1） */
    dw[6]  = (u32)(u64)buf;
    dw[7]  = (u32)((u64)buf >> 32);
    dw[10] = (u32)slba;                               /* SLBA 低 32 位 */
    dw[11] = (u32)(slba >> 32);                       /* SLBA 高 32 位 */
    dw[12] = (nlb - 1u) & 0xFFFFu;                    /* NLB-1（低 16 位） */
    return nvme_io_cmd(dw, 0);
}

/* 初始化控制器：等可配置 → 清寄存器 → 配 AQA/ASQ/ACQ → CC.EN=1 → 等 CSTS.RDY
   返回 0 = 就绪；-1 = 没找到；-2 = 队列分配失败；-3 = 控制器致命错误；-4 = 就绪超时 */
static int nvme_init(void) {
    g_nvme.bar = nvme_find();
    if (!g_nvme.bar) return -1;

    /* 1) 分配 Admin 队列（page_alloc：4KB 对齐 + 物理连续）
          ASQ: 64 项 × 64B = 4096B = 1 页
          ACQ: 64 项 × 16B = 1024B → 1 页够（余下不用） */
    u64 *asq = (u64*)page_alloc(1);
    u64 *acq = (u64*)page_alloc(1);
    if (!asq || !acq) return -2;
    for (u32 i = 0; i < 512; i++) { asq[i] = 0; acq[i] = 0; }

    g_nvme.asq = asq;
    g_nvme.acq = acq;
    g_nvme.sq_tail = 0;
    g_nvme.cq_head = 0;
    g_nvme.cq_phase = 1;        /* ★ 初始相位 1 */
    g_nvme.cid = 1;             /* CID 从 1 起（0 保留） */

    /* 2) 若控制器已被使能（EN=1），先关掉并等 RDY=0
          —— 使能状态下改 AQA/ASQ/ACQ 是未定义行为（规范要求先禁用） */
    if (nvme_read32(g_nvme.bar, NVME_CC) & 1u) {
        nvme_write32(g_nvme.bar, NVME_CC, 0);
        for (u32 i = 0; i < 1000000u && (nvme_read32(g_nvme.bar, NVME_CSTS) & 1u); i++) {
            __asm__ volatile("pause");
        }
    }
    /* 等控制器可配置（CSTS.RDY 先应为 0；有些固件起来就是 1） */
    for (u32 i = 0; i < 100000u && (nvme_read32(g_nvme.bar, NVME_CSTS) & 1u); i++) {
        __asm__ volatile("pause");
    }

    /* 3) 清 AQA / ASQ / ACQ（先归零再配，避免旧值干扰） */
    nvme_write32(g_nvme.bar, NVME_AQA, 0);
    nvme_write64(g_nvme.bar, NVME_ASQ, 0);
    nvme_write64(g_nvme.bar, NVME_ACQ, 0);

    /* 4) 队列深度：ASQS=63（bit0-11）, ACQS=63（bit16-27） */
    nvme_write32(g_nvme.bar, NVME_AQA,
                 (u32)NVME_ADMIN_QD | ((u32)NVME_ADMIN_QD << 16));

    /* 5) 队列基址（identity map：虚拟地址 == 物理地址） */
    nvme_write64(g_nvme.bar, NVME_ASQ, (u64)g_nvme.asq);
    nvme_write64(g_nvme.bar, NVME_ACQ, (u64)g_nvme.acq);

    /* 6) CC = EN(bit0) | IOSQES=6(bits19:16) | IOCQES=4(bits23:20)
          = 0x00460001（CSS=0 → NVMe 命令集；MPS=0 → 4KB 页）
          ★ 之前写成 (6<<8)|(4<<4) 是错的：那会让 IOSQES/IOCQES 全为 0，
            控制器按 2^0=1 字节读 SQE → 命令收不全 → IDENTIFY 永远超时 */
    nvme_write32(g_nvme.bar, NVME_CC, 0x00460001u);

    /* 7) 等 CSTS.RDY = 1（bit0）；CFS(bit1)=1 表示控制器致命错误 */
    for (u32 i = 0; i < 2000000u; i++) {
        u32 csts = nvme_read32(g_nvme.bar, NVME_CSTS);
        if (csts & 1u) { g_nvme.ready = 1; return 0; }
        if (csts & 2u) return -3;
        __asm__ volatile("pause");
    }
    return -4;
}
