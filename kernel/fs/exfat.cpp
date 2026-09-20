/* fs/exfat.c — exFAT 只读驱动（正确版）
   修正原版 WinDOS exfat.c 的坑：
   1. 文件名在 0xC1 条目（UTF-16）——不是 0x85 的 8+3 字段
   2. 首簇/大小在 0xC0 流条目（offset 20/24）——不是 0x85
   3. 分配位图是文件（0x81）——不是 FAT 区（FAT 区只在 fat_length>0 时用于簇链）
   4. 簇号从 2 开始——cluster_to_lba 要 -2（原版漏了）
   5. 文件可能碎片化——读取走簇链（FAT 表或连续） */
#include "exfat.h"
#include "ata.h"

/* freestanding 内核没有 string.h——自实现。
   memcpy 必须全局（extern "C"）：编译器优化（结构拷贝等）会生成外部
   memcpy 调用——static/宏绕不过（链接 undefined） */
extern "C" void *memcpy(void *d, const void *s, unsigned long n) {
    unsigned char *p = (unsigned char*)d;
    const unsigned char *q = (const unsigned char*)s;
    while (n--) *p++ = *q++;
    return d;
}
static int xstrcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static void *xmemset(void *d, int v, unsigned long n) {
    unsigned char *p = (unsigned char*)d;
    while (n--) *p++ = (unsigned char)v;
    return d;
}
#define strcmp xstrcmp
#define memset xmemset

/* 真盘读写：走 ata.h 的 ATA PIO（不再用内存盘宏顶替——那只是解析逻辑的测试替身） */

/* 分区起始 LBA 由 MBR 分区表解析得到（不再硬编码——diskpart 的
   MBR 分区从 LBA 128 起，GPT 从 2048 起——必须解析） */

static struct EXFAT_BOOT g_boot;
static int  g_ok = 0;
static u32  g_bps, g_spc, g_bpc;   /* 每扇区/每簇字节、每簇扇区 */

static u8 g_tmp[8192];             /* 临时簇缓冲（最大 8KB——16 扇区/簇） */
static u32 g_part_lba = 0;            /* 分区起始 LBA（MBR 解析） */
static int g_ch = 0, g_dv = 0;      /* 探测到的盘位（通道×主从） */

/* 引导盘位置由引导器经 UEFI DevicePath 问到并传进来：
   控制器类型 / 通道 / 盘位 / 分区起始 LBA——内核不再猜盘、不再暴力试 4 个位置。
   未设置或类型不支持时，回退到原来的暴力探测。 */
static int g_dev_forced = 0;
void exfat_set_dev(u32 ctrl_kind, u32 ch, u32 dv, u32 part_lba, u32 part_size) {
    (void)part_size;
    if (ctrl_kind == 1 && ch <= 1 && dv <= 1 && part_lba != 0) {  /* 1 = ATA/IDE（端口 0x1F0/0x170）*/
        g_ch = (int)ch; g_dv = (int)dv; g_part_lba = part_lba; g_dev_forced = 1;
    }
}

/* ---- 簇 → LBA（★ 簇号从 2 开始——原版漏了 -2！）---- */
static u32 cluster_to_lba(u32 cluster)
{
    return g_part_lba
         + g_boot.cluster_heap_offset
         + (cluster - 2) * g_spc;
}

/* ---- 读簇 ---- */
static int read_cluster(u32 cluster, u8 *buf)
{
    u32 lba = cluster_to_lba(cluster);
    for (u32 i = 0; i < g_spc; i++) {
        if (ide_read_sector(g_ch, g_dv, lba + i, 1, buf + i * g_bps) != 0) return -1;
    }
    return 0;
}

/* ---- 下一个簇（fat_length>0 用 FAT 表（32位项）；==0 连续）---- */
static u32 next_cluster(u32 cluster)
{
    if (g_boot.fat_length > 0) {
        u8 buf[512];
        u64 fat_byte = (u64)cluster * 4;
        u32 lba = g_part_lba + g_boot.fat_offset + fat_byte / g_bps;
        if (ide_read_sector(g_ch, g_dv, lba, 1, buf) != 0) return 0xFFFFFFFF;
        u32 v;
        memcpy(&v, buf + (fat_byte % g_bps), 4);
        return v;                    /* 0xFFFFFFFF = 链尾 */
    }
    return cluster + 1;              /* 无 FAT → 文件连续 */
}

/* ---- 解析一个目录簇的 Entry Set 组 ---- */
static int parse_dir_cluster(u8 *cb, struct EXFAT_FILE_INFO *out, int max)
{
    int count = 0;
    u32 off = 0;
    while (off < g_bpc && count < max) {
        u8 type = cb[off];
        if (type == EXFAT_EOD) break;                 /* 目录结束 */
        if (type == EXFAT_FILE) {
            /* ── 组开始：0x85 → 0xC0 → 0xC1×n ── */
            struct EXFAT_FILE_INFO fi;
            memset(&fi, 0, sizeof(fi));
            fi.is_dir = (cb[off + 2] & 0x10) ? 1 : 0; /* 属性位 0x10 = 目录 */
            u32 p = off + 32;
            u32 name_total = 0;                        /* 名字字符数（0xC0 offset3） */
            int  name_written = 0;
            while (p < g_bpc) {
                u8 t2 = cb[p];
                if (t2 == EXFAT_STREAM) {
                    memcpy(&fi.first_cluster, cb + p + 20, 4);  /* 首簇 */
                    memcpy(&fi.data_len,      cb + p + 24, 8);  /* 数据长度 */
                    name_total = cb[p + 3];                    /* NameLength */
                } else if (t2 == EXFAT_NAME) {
                    /* UTF-16 → 低字节 ASCII 近似（中文后续） */
                    for (int i = 0; i < 15 && name_written < 255; i++) {
                        u16 ch = cb[p + 2 + i * 2] | (cb[p + 2 + i * 2 + 1] << 8);
                        if (ch == 0) break;
                        fi.name[name_written++] = (char)(ch & 0xFF);
                    }
                } else {
                    break;                               /* 新条目 → 组结束 */
                }
                p += 32;
            }
            (void)name_total;
            fi.name[name_written] = 0;
            out[count++] = fi;
            off = p;                                     /* 跳到组尾 */
        } else {
            off += 32;                                   /* 位图/卷标等——跳过 */
        }
    }
    return count;
}

/* ---- 列目录（遍历目录的所有簇）---- */
int exfat_list_dir(u32 dir_cluster, struct EXFAT_FILE_INFO *out, int max)
{
    if (!g_ok) return 0;
    int count = 0;
    u32 cluster = dir_cluster;
    while (cluster >= 2 && cluster != 0xFFFFFFFF) {
        if (read_cluster(cluster, g_tmp) != 0) break;
        int n = parse_dir_cluster(g_tmp, out + count, max - count);
        count += n;
        if (n == 0) break;               /* 遇到 EOD */
        cluster = next_cluster(cluster);
    }
    return count;
}

/* ---- 查找文件（ASCII 名——UTF-16 低字节）---- */
int exfat_find(u32 dir_cluster, const char *fname, struct EXFAT_FILE_INFO *out)
{
    if (!g_ok) return -1;
    struct EXFAT_FILE_INFO infos[64];
    int n = exfat_list_dir(dir_cluster, infos, 64);
    for (int i = 0; i < n; i++) {
        if (strcmp(infos[i].name, fname) == 0) {
            *out = infos[i];
            return 0;
        }
    }
    return -1;
}

/* ---- 读文件（走簇链——支持碎片）---- */
int exfat_read_file(u32 first_cluster, u64 size, u8 *buf)
{
    if (!g_ok) return -1;
    u64 remaining = size;
    u32 cluster = first_cluster;
    while (remaining > 0) {
        if (cluster < 2 || cluster == 0xFFFFFFFF) return -1;   /* 链断/越界 */
        if (read_cluster(cluster, g_tmp) != 0) return -1;
        u64 chunk = remaining < g_bpc ? remaining : g_bpc;
        memcpy(buf, g_tmp, (u32)chunk);
        buf += chunk;
        remaining -= chunk;
        cluster = next_cluster(cluster);
    }
    return 0;
}

/* ---- 写文件（内存盘测试版：创建/覆盖一个单簇文件）----
   流程（大白话）：位图找空簇 → 写数据进簇 → 位图置1 → 目录加登记卡
   注意：只支持单簇文件（size <= 一簇）；位图簇用根目录 0x81 条目 */
int exfat_write_file(const char *fname, const u8 *data, u64 size)
{
    if (!g_ok) return -1;
    int nlen = 0;
    while (fname[nlen] && nlen < 255) nlen++;
    if (nlen == 0 || size > g_bpc) return -1;   /* 简化：单簇 */

    /* 1. 读根目录找 0x81（位图条目）→ 位图簇 */
    u8 rd_buf[8192];
    if (read_cluster(2, rd_buf) != 0) return -1;
    u32 bm_cluster = 0;
    for (u32 o = 0; o < g_bpc; o += 32) {
        if (rd_buf[o] == 0x81) {
            memcpy(&bm_cluster, rd_buf + o + 20, 4);
            break;
        }
        if (rd_buf[o] == 0x00) break;
    }
    if (bm_cluster == 0) return -1;

    /* 2. 位图找空簇（bit=0——"空房间"） */
    u8 bm[512];
    if (read_cluster(bm_cluster, bm) != 0) return -1;
    u32 free_cluster = 0;
    for (u32 cc = 2; cc < g_boot.cluster_count; cc++) {
        if ((bm[cc / 8] & (1 << (cc % 8))) == 0) { free_cluster = cc; break; }
    }
    if (free_cluster == 0) return -1;   /* 满盘 */

    /* 3. 写数据进簇（真盘：按扇区读-改-写——小文件不破坏同扇区的其它数据） */
    {
        u32 lba0 = cluster_to_lba(free_cluster);
        u8 sec[512];
        for (u32 o = 0; o < (u32)size; o += 512) {
            u32 n = (u32)size - o; if (n > 512) n = 512;
            if (ide_read_sector(g_ch, g_dv, lba0 + o / 512, 1, sec) != 0) return -2;
            memcpy(sec, data + o, n);
            if (ide_write_sector(g_ch, g_dv, lba0 + o / 512, 1, sec) != 0) return -3;
        }
    }

    /* 4. 位图置 1 + 写回（"房间标记已用"） */
    bm[free_cluster / 8] |= (u8)(1 << (free_cluster % 8));
    if (ide_write_sector(g_ch, g_dv, cluster_to_lba(bm_cluster), 1, bm) != 0) return -4;   /* 真盘写位图 */

    /* 5. 目录加条目组（0x85 文件 + 0xC0 流 + 0xC1 名字——"登记卡"） */
    u32 off = 0;
    while (rd_buf[off] != 0x00 && off < g_bpc) off += 32;   /* 找目录末尾 EOD */
    if (off + 96 > g_bpc) return -1;
    /* 0x85 文件条目 */
    rd_buf[off + 0] = 0x85;
    rd_buf[off + 2] = 0x20;                          /* 属性：档案 */
    /* 0xC0 流条目 */
    rd_buf[off + 32 + 0] = 0xC0;
    rd_buf[off + 32 + 3] = (u8)nlen;                 /* 名字长度 */
    memcpy(rd_buf + off + 32 + 8, &size, 8);         /* 有效数据长度 */
    memcpy(rd_buf + off + 32 + 20, &free_cluster, 4);/* 首簇 */
    memcpy(rd_buf + off + 32 + 24, &size, 8);        /* 数据长度 */
    /* 0xC1 名字条目（UTF-16——ASCII 低字节） */
    rd_buf[off + 64 + 0] = 0xC1;
    for (int i = 0; i < nlen; i++) {
        rd_buf[off + 64 + 2 + i * 2] = (u8)fname[i];
        rd_buf[off + 64 + 2 + i * 2 + 1] = 0;
    }
    rd_buf[off + 96] = 0x00;                         /* 新 EOD */
    /* 目录簇写回（真盘） */
    {
        u32 dn = g_bpc / 512; if (dn == 0) dn = 1;
        if (ide_write_sector(g_ch, g_dv, cluster_to_lba(2), dn, rd_buf) != 0) return -5;
    }
    return 0;
}

/* ---- 初始化 ---- */
int exfat_init(void)
{
    u8 buf[512], mbr[512];
    u32 part_lba = 0;
    /* 自动探测盘位：试 4 个组合（Primary/Secondary × master/slave）——
       读 MBR（LBA 0）→ 解析第一分区起始 → 读分区引导扇区验 EXFAT 签名 */
    int found = 0;
    if (g_dev_forced) {
        /* UEFI 给的位置：只验这一个点（读分区引导扇区确认 EXFAT 签名）*/
        if (ide_read_sector(g_ch, g_dv, g_part_lba, 1, buf) == 0
            && buf[3]=='E'&&buf[4]=='X'&&buf[5]=='F'&&buf[6]=='A'&&buf[7]=='T'
            && buf[510]==0x55 && buf[511]==0xAA) found = 1;
        else return -3;   /* 位置已知但验不过——不再瞎试其它盘位（错盘比慢更糟）*/
    }
    for (int ch = 0; ch <= 1 && !found; ch++) {
        for (int dv = 0; dv <= 1 && !found; dv++) {
            if (ide_read_sector(ch, dv, 0, 1, mbr) != 0) continue;
            if (mbr[510] != 0x55 || mbr[511] != 0xAA) continue;   /* MBR 签名 */
            part_lba = *(u32*)(mbr + 454);                         /* 第一分区起始 LBA */
            if (part_lba == 0) continue;
            if (ide_read_sector(ch, dv, part_lba, 1, buf) != 0) continue;
            if (buf[3]=='E'&&buf[4]=='X'&&buf[5]=='F'&&buf[6]=='A'&&buf[7]=='T') {
                if (buf[510] != 0x55 || buf[511] != 0xAA) continue;  /* ★ 引导签名 0xAA55——防误判 */
                g_ch = ch; g_dv = dv;
                g_part_lba = part_lba;
                found = 1;
            }
        }
    }
    if (!found) return -2;   /* 4 个盘位都读不到 ExFAT */
    /* 逐字段拷贝引导参数（小端——避免结构对齐问题） */
    memcpy(&g_boot.partition_offset,    buf + 0x40, 8);
    memcpy(&g_boot.volume_length,       buf + 0x48, 8);
    memcpy(&g_boot.fat_offset,          buf + 0x50, 4);
    memcpy(&g_boot.fat_length,          buf + 0x54, 4);
    memcpy(&g_boot.cluster_heap_offset, buf + 0x58, 4);
    memcpy(&g_boot.cluster_count,       buf + 0x5C, 4);
    memcpy(&g_boot.root_dir_cluster,    buf + 0x60, 4);
    memcpy(&g_boot.serial,              buf + 0x64, 4);
    g_boot.bps_shift = buf[0x6C];
    g_boot.spc_shift = buf[0x6D];
    g_boot.num_fats  = buf[0x6E];
    g_bps = 1u << g_boot.bps_shift;
    g_spc = 1u << g_boot.spc_shift;
    g_bpc = g_bps * g_spc;
    if (g_bpc > sizeof(g_tmp)) return -4;   /* 簇太大（>8KB）——不支持 */
    g_ok = 1;
    return 0;
}
