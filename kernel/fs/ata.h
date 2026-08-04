/* fs/ata.h — ATA PIO 磁盘驱动（QEMU IDE 盘，LBA28 读扇区）
   两个 IDE 通道：Primary 0x1F0-0x1F7 / Secondary 0x170-0x177
   channel=0/1 选通道，drive=0/1 选主/从盘（0x1F6 bit4）
   流程：等不忙 → 写 LBA/扇区数 → 命令 0x20(读) → 等 DRQ → 读 256×u16 */
#pragma once
#include "../boot/types.h"
#include "../drivers/io.h"

static inline u16 in16(u16 p) { u16 v; __asm__("inw %1,%0":"=a"(v):"dN"(p)); return v; }

/* 等待磁盘不忙（BSY 清）——channel 选端口 */
static inline int ata_wait_bsy(int channel) {
    u16 stat = channel ? 0x177 : 0x1F7;
    for (int i = 0; i < 200000; i++) {
        if ((in8(stat) & 0x80) == 0) return 0;
    }
    return -1;   /* 超时 */
}

/* 读扇区（LBA28，最多一次 count 个——每个 512B）
   channel: 0=Primary(0x1F0) 1=Secondary(0x170)
   drive:   0=master 1=slave */
static inline int ide_read_sector(int channel, int drive, u32 lba, u32 count, u8 *buf) {
    u16 base = channel ? 0x170 : 0x1F0;
    u16 data = base, stat = base + 7, drv = base + 6;
    u16 sct = base + 2, lbal = base + 3, lbam = base + 4, lbah = base + 5;
    for (u32 s = 0; s < count; s++) {
        if (ata_wait_bsy(channel) != 0) return -1;
        /* 选盘 + LBA 高位（XJ380: drive_select | 0x40 | lba>>24） */
        out8(drv, (drive ? 0xF0 : 0xE0) | ((lba >> 24) & 0x0F));
        for (int i = 0; i < 4; i++) in8(stat);   /* 选盘后读 4 次——状态稳定（XJ380） */
        out8(sct, 1);
        out8(lbal, lba & 0xFF);
        out8(lbam, (lba >> 8) & 0xFF);
        out8(lbah, (lba >> 16) & 0xFF);
        out8(stat, 0x20);                        /* READ SECTOR(S) */
        int got = 0;
        for (int i = 0; i < 1000000; i++) {
            u8 st = in8(stat);
            if (st & 0x08) { got = 1; break; }   /* DRQ */
            if (st == 0 || st == 0xFF) return -2; /* 无盘（XJ380 检查） */
            if (st & 0x01) return -3;            /* 错误位 */
        }
        if (!got) return -4;                     /* DRQ 超时 */
        for (int i = 0; i < 256; i++) {
            u16 v = in16(data);
            buf[s * 512 + i * 2]     = v & 0xFF;
            buf[s * 512 + i * 2 + 1] = v >> 8;
        }
        lba++;
    }
    return 0;
}
