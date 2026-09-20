/* fs/exfat.h — exFAT 文件系统（结构 + API）
   ★ 修正原版 WinDOS exfat.c 的坑（原版按 FAT32 思维写 ExFAT）：
     - 文件名是 UTF-16（0xC1 条目，15 字符/条，长名多条目）——不是 8+3
     - 起始簇/大小在 0xC0 流条目（offset 20/24）——不是 0x85 的 20/24
     - 分配位图是"文件"（0x81 条目指向的簇）——不是 FAT 区
     - 文件可能碎片化——读取要逐簇追踪（位图/FAT 链）
   API 形状保持原版（char* 名——UTF-16 低字节近似 ASCII——先跑通） */
#pragma once
#include "../boot/types.h"

/* exFAT 引导扇区（关键字段——偏移对应 ExFAT 规范） */
struct EXFAT_BOOT {
    u8  jump[3];
    u8  oem[8];              /* "EXFAT   " */
    u8  rsv1[53];
    u64 partition_offset;    /* 0x40 */
    u64 volume_length;       /* 0x48 */
    u32 fat_offset;          /* 0x50 */
    u32 fat_length;          /* 0x54 */
    u32 cluster_heap_offset; /* 0x58 */
    u32 cluster_count;       /* 0x5C */
    u32 root_dir_cluster;    /* 0x60 */
    u32 serial;              /* 0x64 */
    u16 fs_revision;         /* 0x68 */
    u16 volume_flags;        /* 0x6A */
    u8  bps_shift;           /* 0x6C——扇区字节数 = 1<<shift */
    u8  spc_shift;           /* 0x6D——每簇扇区数 = 1<<shift */
    u8  num_fats;            /* 0x6E */
};

/* 目录项类型 */
#define EXFAT_EOD    0x00   /* 目录结束 */
#define EXFAT_BITMAP 0x81   /* 分配位图 */
#define EXFAT_UPCASE 0x82   /* 大小写表 */
#define EXFAT_VOLUME 0x83   /* 卷标 */
#define EXFAT_FILE   0x85   /* 文件（属性/时间戳） */
#define EXFAT_STREAM 0xC0   /* 流（首簇 offset20 / 数据长度 offset24） */
#define EXFAT_NAME   0xC1   /* 文件名（UTF-16，offset2 起，15 字符/条） */

/* 目录项组(Entry Set)解析结果 */
struct EXFAT_FILE_INFO {
    u32 first_cluster;      /* 0xC0 流条目的首簇 */
    u64 data_len;           /* 0xC0 流条目的数据长度 */
    char name[300];         /* 文件名（UTF-16 → 低字节 ASCII 近似） */
    u8  is_dir;             /* 0x85 文件条目的目录属性位(0x10) */
};

/* ---- API ---- */
void exfat_set_dev(u32 ctrl_kind, u32 ch, u32 dv, u32 part_lba, u32 part_size);  /* 引导器给的盘位置 */
int  exfat_init(void);       /* 读引导扇区 + 解析参数（先验 "EXFAT"） */
int  exfat_list_dir(u32 dir_cluster, struct EXFAT_FILE_INFO *out, int max);
int  exfat_find(u32 dir_cluster, const char *fname, struct EXFAT_FILE_INFO *out);
int  exfat_read_file(u32 first_cluster, u64 size, u8 *buf);
int  exfat_write_file(const char *fname, const u8 *data, u64 size);  /* 写（内存盘测试版） */
