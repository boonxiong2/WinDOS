#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""tools/mkmemdisk.py — 生成 esp/memdisk.img（ExFAT + KERNEL.BIN）

为什么需要它：
  引导器（src/main.rs）不内嵌内核——它用 UEFI 文件协议从 ESP 根目录读
  memdisk.img，再按 ExFAT 解析出 KERNEL.BIN 加载。而 esp/ 在 .gitignore 里，
  所以 clone 下来的仓库没有这个镜像 —— 跑一次本脚本就有了（build.bat 会自动调用）。

镜像布局（必须与 src/main.rs 的 exfat_find_kern 完全一致）：
  LBA0        : MBR，分区项 @0x1C2 类型 0x07、起始 LBA @0x1C6、扇区数 @0x1CA、0x55AA
  LBA128 起   : ExFAT 卷（BPB 里 FAT_OFF @0x50、簇堆 @0x58、根目录簇 @0x60、spc_shift @0x72）
  根目录      : 0x81 文件条目 + 0x85 流扩展（首簇 @+16、数据长度 @+24）+ 0xC0 名字条目
  数据簇      : HELLO.TXT（测试用，可选）与 KERNEL.BIN（内核本体）

上限：引导器只有 0x40000(256KB) 缓冲存 memdisk.img，整个镜像超了就 "read memdisk fail"。
    内核变大导致超限时，改 src/main.rs 的 mb_total 并同步本文件的 LIMIT。
"""
import os
import struct
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
KERN = os.path.join(ROOT, "kernel", "kernel.kern")
OUT = os.path.join(ROOT, "esp", "memdisk.img")

CLUSTER = 512        # 簇大小（必须 512 → BPB spc_shift = 0）
FAT_OFF = 8          # FAT 起始扇区（卷内偏移）
FAT_LEN = 8          # FAT 长度（扇区）
HEAP = 16            # 簇堆起始扇区
PART_LBA = 128       # 分区起始 LBA
LIMIT = 0x40000      # 引导器缓冲 256KB


def file_entry(buf, off, data_len, alloc, first_cluster):
    """0x81 文件目录条目 + 0x85 流扩展（引导器按这个位置读长度和首簇）"""
    buf[off] = 0x81                       # 文件条目
    struct.pack_into("<Q", buf, off + 16, data_len)
    struct.pack_into("<Q", buf, off + 24, alloc)
    s = off + 32                          # 流扩展紧跟其后
    buf[s] = 0x85
    struct.pack_into("<Q", buf, s + 8, data_len)
    struct.pack_into("<I", buf, s + 16, first_cluster)   # 首簇
    struct.pack_into("<Q", buf, s + 24, data_len)        # 数据长度
    return off + 32


def name_entry(buf, off, name):
    """0xC0|n 文件名条目（UTF-16LE，n = 字符数）"""
    ch = name.encode("utf-16le")
    n = len(name)
    buf[off] = 0xC0 | (n & 0x3F)          # bit6=1 = secondary（引导器按 nt&0xC0==0xC0 判断）
    buf[off + 1] = n
    for i in range(min(15, n)):
        struct.pack_into("<H", buf, off + 2 + 2 * i, ch[2 * i] | (ch[2 * i + 1] << 8))
    return off + 32


def build(kern):
    # 簇分配：2 = 根目录，3 = HELLO.TXT，4.. = KERNEL.BIN
    n_kern = (len(kern) + CLUSTER - 1) // CLUSTER
    cluster_count = n_kern + 4
    # 卷长 = 簇堆起始扇区 + 簇堆本身（簇大小 512B → 1 簇 = 1 扇区）。
    # 注意：上限 256KB 是对"整个镜像"（含 MBR + 分区前缀）而言的，别把前缀漏算。
    vol_sectors = HEAP + cluster_count

    bpb = bytearray(512)
    bpb[0:3] = b"\xeb\x76\x90"            # 跳转指令（ExFAT 规范要求）
    bpb[3:11] = b"EXFAT   "               # 文件系统标识
    struct.pack_into("<Q", bpb, 0x40, PART_LBA)      # 分区偏移
    struct.pack_into("<Q", bpb, 0x48, vol_sectors)   # 卷长度
    struct.pack_into("<I", bpb, 0x50, FAT_OFF)
    struct.pack_into("<I", bpb, 0x54, FAT_LEN)
    struct.pack_into("<I", bpb, 0x58, HEAP)          # 簇堆起始扇区
    struct.pack_into("<I", bpb, 0x5C, cluster_count)
    struct.pack_into("<I", bpb, 0x60, 2)             # 根目录簇号
    struct.pack_into("<Q", bpb, 0x64, 0x12345678)
    bpb[0x72] = 0                         # spc_shift = 0 → 簇 = 512B
    bpb[0x73] = 1
    lbl = "WINDOX".encode("utf-16le")
    bpb[0x88:0x88 + len(lbl)] = lbl
    bpb[0xA2] = 0x01

    img = bytearray(bpb)
    img += b"\x00" * (FAT_OFF * 512 - len(img))

    fat = bytearray(FAT_LEN * 512)
    used = {0, 1, 2, 3} | {4 + i for i in range(n_kern)}
    for c in used:
        fat[c // 8] |= 1 << (c % 8)
    img += fat
    img += b"\x00" * (HEAP * 512 - len(img))

    hello = b"HELLO FROM EXFAT"
    root = bytearray(CLUSTER)
    name_entry(root, file_entry(root, 0x00, len(hello), CLUSTER, 3) + 32, "HELLO.TXT")
    name_entry(root, file_entry(root, 0x60, len(kern), len(kern), 4) + 32, "KERNEL.BIN")
    img += root
    img += hello.ljust(CLUSTER, b"\x00")
    img += kern
    while len(img) < vol_sectors * 512:
        img += b"\x00" * 512

    mbr = bytearray(512)
    mbr[0x1C2] = 0x07                     # 分区类型
    struct.pack_into("<I", mbr, 0x1C6, PART_LBA)
    struct.pack_into("<I", mbr, 0x1CA, len(img) // 512)
    mbr[0x1FE:0x200] = b"\x55\xaa"
    return bytes(mbr) + b"\x00" * (PART_LBA * 512 - 512) + bytes(img)


def main():
    if not os.path.exists(KERN):
        print(f"[mkmemdisk] 找不到 {KERN} —— 先跑 build.bat 编译内核")
        return 1
    kern = open(KERN, "rb").read()
    image = build(kern)
    size = len(image)
    print(f"[mkmemdisk] KERNEL.BIN {len(kern)} B → memdisk.img {size} B ({size/1024:.1f} KB)")
    if size > LIMIT:
        print(f"[mkmemdisk] 超限！引导器缓冲是 0x{LIMIT:X} ({LIMIT//1024} KB)。")
        print("[mkmemdisk] 处理：调大 src/main.rs 的 mb_total，并同步本文件的 LIMIT。")
        return 1
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    open(OUT, "wb").write(image)
    print(f"[mkmemdisk] 已写出 {OUT}（余量 {LIMIT - size} B）")
    return 0


if __name__ == "__main__":
    sys.exit(main())
