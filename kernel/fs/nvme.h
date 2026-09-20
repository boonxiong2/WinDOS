/* fs/nvme.h — NVMe 驱动（QEMU 模拟——PCIe NVMe SSD）
   流程：PCI 枚举(0xCF8/0xCFC) → 找 NVMe → BAR0(MMIO 寄存器)
        → 初始化(CC/AQA/队列) → IDENTIFY → IO 队列读(PRP)
   第一版：PCI 枚举 + BAR0/CAP 探测（诊断用） */
#pragma once
#include "../boot/types.h"
#include "../drivers/io.h"

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
#define NVME_CC    0x14   /* 控制器配置：bit0=EN bit4-7=IOCQES bit8-11=IOSQES bit16-19=MPS */
#define NVME_CSTS  0x1C   /* 状态：bit0=RDY */
#define NVME_AQA   0x24   /* Admin 队列属性：bit0-11=ASQS bit16-27=ACQS */
#define NVME_ASQ   0x28   /* Admin 提交队列基址（物理，页对齐） */
#define NVME_ACQ   0x30   /* Admin 完成队列基址 */

static inline u32 nvme_read32(u64 bar, u32 off) { return *(volatile u32*)(u64)(bar + off); }
static inline void nvme_write32(u64 bar, u32 off, u32 v) { *(volatile u32*)(u64)(bar + off) = v; }

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
