/* fs/exfat.c — exFAT 只读驱动（正确版）
   修正原版 WinDOS exfat.c 的坑：
   1. 文件名在 0xC1 条目（UTF-16）——不是 0x85 的 8+3 字段
   2. 首簇/大小在 0xC0 流条目（offset 20/24）——不是 0x85
   3. 分配位图是文件（0x81）——不是 FAT 区（FAT 区只在 fat_length>0 时用于簇链）
   4. 簇号从 2 开始——cluster_to_lba 要 -2（原版漏了）
   5. 文件可能碎片化——读取走簇链（FAT 表或连续） */
#include "exfat.h"
#include "ata.h"
#include "nvme.h"        /* 块层：NVMe 就绪时优先走 NVMe，否则回落 ATA PIO */
#include "sys/serial.h"
#include "sys/stdio.h"


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

/* ---- 块层：所有扇区 I/O 统一过这两个函数 ----
   NVMe 就绪时优先走 NVMe；用 4KB 对齐的 bounce 缓冲（PRP1 永远页对齐、
   每次最多 8 扇区 = 一页），避免调用方传栈上非对齐指针。
   NVMe 未就绪 → 回落 ATA PIO（老路径，行为不变）。 */
static u8 g_bounce[4096] __attribute__((aligned(4096)));

static int sec_rd(u32 lba, u32 cnt, u8 *buf) {
    if (g_nvme.ready) {
        if (cnt > 8) return -1;
        if (nvme_rw(0x02, 1, (u64)lba, cnt, g_bounce) != 0) return -1;
        memcpy(buf, g_bounce, cnt * 512);
        return 0;
    }
    return ide_read_sector(g_ch, g_dv, lba, cnt, buf);
}

static int sec_wr(u32 lba, u32 cnt, const u8 *buf) {
    if (g_nvme.ready) {
        if (cnt > 8) return -1;
        memcpy(g_bounce, buf, cnt * 512);
        return nvme_rw(0x01, 1, (u64)lba, cnt, g_bounce) == 0 ? 0 : -1;
    }
    return ide_write_sector(g_ch, g_dv, lba, cnt, buf);
}

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
        if (sec_rd(lba + i, 1, buf + i * g_bps) != 0) return -1;
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
        if (sec_rd(lba, 1, buf) != 0) return 0xFFFFFFFF;
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
                    fi.no_fat_chain = (cb[p + 1] >> 1) & 1;     /* NoFatChain：簇连续 */
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



/* ================= 写文件（多簇 + 覆盖 + 校验和）=================
   分配策略：连续 ncl 个空簇（NoFatChain=1，不维护 FAT）
   断电安全序：写数据 → 位图落盘 → 最后写目录条目（提交点） */
static u16 ex_rot16(u16 h, u8 b) { return (u16)(((h << 15) | (h >> 1)) + b); }

/* 0x85 组的 SetChecksum：覆盖 (SecondaryCount+1)*32 字节，跳过自身 byte2-3 */
static u16 ex_set_checksum(const u8 *set, u32 secondary) {
    u32 n = 32u * (secondary + 1u);
    u16 s = 0;
    for (u32 i = 0; i < n; i++) { if (i == 2 || i == 3) continue; s = ex_rot16(s, set[i]); }
    return s;
}
/* 0xC0 的 NameHash：对 UTF-16LE 名字逐字节做同样的旋转加 */
static u16 ex_name_hash(const char *name, int nlen) {
    u16 h = 0;
    for (int i = 0; i < nlen; i++) { h = ex_rot16(h, (u8)name[i]); h = ex_rot16(h, 0); }
    return h;
}

int exfat_write_file(const char *fname, const u8 *data, u64 size)
{
    if (!g_ok) return -1;
    int nlen = 0;
    while (fname[nlen] && nlen < 255) nlen++;
    if (nlen == 0) return -2;
    if (nlen > 30) return -7;                      /* 最多 2 个名字条目 */
    int nname = (nlen + 14) / 15;

    u8 rd_buf[8192];
    if (read_cluster(g_boot.root_dir_cluster, rd_buf) != 0) return -3;

    /* 位图簇（根目录里的 0x81 条目）*/
    u32 bm_cluster = 0;
    for (u32 o = 0; o < g_bpc; o += 32) {
        if (rd_buf[o] == 0x81) { memcpy(&bm_cluster, rd_buf + o + 20, 4); break; }
        if (rd_buf[o] == 0x00) break;
    }
    if (bm_cluster == 0) return -4;

    u8 bm[66536];
    if (read_cluster(bm_cluster, bm) != 0) return -1;

    /* 扫目录：找同名组（覆盖）或 EOD（新建）*/
    u32 eod = 0, slot = 0;
    for (u32 o = 0; o + 32 <= g_bpc; ) {
        u8 t = rd_buf[o];
        if (t == 0x00) { eod = o; break; }
        if (t == 0x85) {
            u32 q = o + 32, end = o + 32;
            char nm[64]; int k = 0;
            while (q < g_bpc) {
                u8 t2 = rd_buf[q];
                if (t2 == 0xC0) { end = q; }
                else if (t2 == 0xC1) {
                    end = q;
                    for (int i = 0; i < 15; i++) {
                        u16 ch = (u16)(rd_buf[q + 2 + i * 2] | (rd_buf[q + 3 + i * 2] << 8));
                        if (ch == 0) break;
                        if (k < 63) nm[k++] = (char)(ch & 0xFF);
                    }
                } else break;
                q += 32;
            }
            nm[k] = 0;
            if (k && strcmp(nm, fname) == 0) { slot = o; break; }
            o = end + 32;
            continue;
        }
        o += 32;
    }
    if (slot == 0 && eod == 0) return -6;          /* 目录满（本簇内）*/

    /* 连续分配 ncl 个空簇 */
    u32 ncl = (u32)((size + g_bpc - 1) / g_bpc);
    u32 first = 0;
    if (ncl) {
        u32 run = 0, start = 0;
        for (u32 cc = 2; cc + ncl <= g_boot.cluster_count + 2 && !start; cc++) {
            if ((bm[cc / 8] >> (cc % 8)) & 1) { run = 0; continue; }
            if (++run == ncl) start = cc + 1 - ncl;
        }
        if (!start) return -5;                     /* 没有足够连续空间 */
        first = start;
        for (u32 i = 0; i < ncl; i++) bm[(first + i) / 8] |= (u8)(1 << ((first + i) % 8));
    }

    /* 写数据（整簇顺序；不足一扇区补零）*/
    for (u32 i = 0; i < ncl; i++) {
        for (u32 sec = 0; sec < g_spc; sec++) {
            u64 off = (u64)i * g_bpc + (u64)sec * 512;
            if (off >= size) break;
            u8 sb[512];
            u32 n = (u32)(size - off); if (n > 512) n = 512;
            memcpy(sb, data + off, n);
            if (n < 512) memset(sb + n, 0, 512 - n);
            if (sec_wr(cluster_to_lba(first + i) + sec, 1, sb) != 0) return -3;
        }
    }

    /* 覆盖：释放旧文件占用的簇 */
    if (slot) {
        u32 of = 0; u64 ol = 0;
        memcpy(&of, rd_buf + slot + 32 + 20, 4);
        memcpy(&ol, rd_buf + slot + 32 + 24, 8);
        if (of >= 2) {
            u32 onc = (u32)((ol + g_bpc - 1) / g_bpc);
            for (u32 i = 0; i < onc; i++) {
                u32 c = of + i;
                if (c < g_boot.cluster_count + 2) bm[c / 8] &= (u8)~(1 << (c % 8));
            }
        }
    }

    /* 位图落盘 */
    if (sec_wr(cluster_to_lba(bm_cluster), g_spc, bm) != 0) return -4;

    /* 目录条目 */
    u32 off = slot ? slot : eod;
    u32 sec_count = 1u + (u32)nname;               /* 0xC0 + 0xC1×n */
    if (!slot) {
        if (off + 32u * (1u + sec_count) + 32u > g_bpc) return -6;
        memset(rd_buf + off, 0, 32u * (1u + sec_count));
        rd_buf[off + 0] = 0x85;
    }
    /* 新建和覆盖都要刷新 0x85 头部（覆盖时旧条目可能是别家写的）*/
    rd_buf[off + 0] = 0x85;
    rd_buf[off + 1] = (u8)sec_count;                /* SecondaryCount */
    rd_buf[off + 4] = 0x20; rd_buf[off + 5] = 0x00; /* 属性：档案 */
    u32 st = off + 32;
    rd_buf[st + 0] = 0xC0;
    rd_buf[st + 1] = 0x03;                          /* AllocationPossible | NoFatChain */
    rd_buf[st + 3] = (u8)nlen;
    { u16 nh = ex_name_hash(fname, nlen); memcpy(rd_buf + st + 4, &nh, 2); }
    { u64 v = size; memcpy(rd_buf + st + 8, &v, 8); }     /* ValidDataLength */
    memcpy(rd_buf + st + 20, &first, 4);                  /* FirstCluster */
    { u64 v = size; memcpy(rd_buf + st + 24, &v, 8); }    /* DataLength */
    for (int e = 0; e < nname; e++) {
        u32 np = st + 32u * (u32)(e + 1);
        rd_buf[np + 0] = 0xC1;
        for (int i = 0; i < 15; i++) {
            int idx = e * 15 + i;
            u16 ch = (idx < nlen) ? (u16)(u8)fname[idx] : 0;
            rd_buf[np + 2 + i * 2] = (u8)ch;
            rd_buf[np + 3 + i * 2] = (u8)(ch >> 8);
        }
    }
    u32 eod2 = off + 32u * (1u + sec_count);
    if (eod2 < g_bpc) rd_buf[eod2] = 0x00;
    { u16 ck = ex_set_checksum(rd_buf + off, sec_count); memcpy(rd_buf + off + 2, &ck, 2); }

    { u32 dn = g_bpc / 512; if (dn == 0) dn = 1;
      if (sec_wr(cluster_to_lba(g_boot.root_dir_cluster), dn, rd_buf) != 0) return -5; }
    return 0;
}

/* ---- 按条目读（NoFatChain → 连续簇；否则走 FAT 链）---- */
int exfat_read_ex(const struct EXFAT_FILE_INFO *fi, u8 *buf)
{
    if (!g_ok || !fi) return -1;
    u64 remaining = fi->data_len;
    u32 cluster = fi->first_cluster, idx = 0;
    while (remaining > 0) {
        u32 c = fi->no_fat_chain ? (fi->first_cluster + idx) : cluster;
        if (c < 2 || c == 0xFFFFFFFF) return -1;
        if (read_cluster(c, g_tmp) != 0) return -1;
        u64 chunk = remaining < g_bpc ? remaining : g_bpc;
        memcpy(buf, g_tmp, (u32)chunk);
        buf += chunk; remaining -= chunk; idx++;
        if (!fi->no_fat_chain) cluster = next_cluster(cluster);
    }
    return 0;
}

/* ---- 删除文件：条目组标记 0xE5 + 释放簇 ---- */
int exfat_delete_file(const char *fname)
{
    if (!g_ok) return -1;
    u8 rd_buf[8192];
    if (read_cluster(g_boot.root_dir_cluster, rd_buf) != 0) return -2;
    u32 bm_cluster = 0;
    for (u32 o = 0; o < g_bpc; o += 32) {
        if (rd_buf[o] == 0x81) { memcpy(&bm_cluster, rd_buf + o + 20, 4); break; }
        if (rd_buf[o] == 0x00) break;
    }
    if (!bm_cluster) return -3;

    for (u32 o = 0; o + 32 <= g_bpc; ) {
        u8 t = rd_buf[o];
        if (t == 0x00) break;
        if (t != 0x85) { o += 32; continue; }
        u32 q = o + 32, end = o + 32;
        char nm[64]; int k = 0;
        u32 first = 0; u64 len = 0; u32 secn = rd_buf[o + 1];
        while (q < g_bpc) {
            u8 t2 = rd_buf[q];
            if (t2 == 0xC0) { end = q; memcpy(&first, rd_buf + q + 20, 4); memcpy(&len, rd_buf + q + 24, 8); }
            else if (t2 == 0xC1) {
                end = q;
                for (int i = 0; i < 15; i++) {
                    u16 ch = (u16)(rd_buf[q + 2 + i * 2] | (rd_buf[q + 3 + i * 2] << 8));
                    if (ch == 0) break;
                    if (k < 63) nm[k++] = (char)(ch & 0xFF);
                }
            } else break;
            q += 32;
        }
        nm[k] = 0;
        if (k && strcmp(nm, fname) == 0) {
            /* 释放簇 */
            if (first >= 2 && len) {
                u8 bm[66536];
                if (read_cluster(bm_cluster, bm) == 0) {
                    u32 onc = (u32)((len + g_bpc - 1) / g_bpc);
                    for (u32 i = 0; i < onc; i++) {
                        u32 c = first + i;
                        if (c < g_boot.cluster_count + 2) bm[c / 8] &= (u8)~(1 << (c % 8));
                    }
                    sec_wr(cluster_to_lba(bm_cluster), g_spc, bm);
                }
            }
            /* 条目组全部标记删除（0xE5）*/
            u32 e = o, n = 0;
            while (e <= end && n <= secn) { rd_buf[e] = 0xE5; e += 32; n++; }
            u32 dn = g_bpc / 512; if (dn == 0) dn = 1;
            return sec_wr(cluster_to_lba(g_boot.root_dir_cluster), dn, rd_buf) == 0 ? 0 : -4;
        }
        o = end + 32;
    }
    return -5;   /* 没找到 */
}

/* ---- 初始化 ---- */
int exfat_init(void)
{
    { char d0[80]; ksprintf(d0, "[KERNEL/FS] init enter: ready=%d g=%x\n",
                             g_nvme.ready, (u32)(u64)&g_nvme); out_file_str(d0); }
    u8 buf[512], mbr[512];
    u32 part_lba = 0;
    /* 自动探测盘位：试 4 个组合（Primary/Secondary × master/slave）——
       读 MBR（LBA 0）→ 解析第一分区起始 → 读分区引导扇区验 EXFAT 签名 */
    int found = 0;
    /* NVMe 就绪 → 优先在 NVMe 上找 exFAT（块层 sec_rd 会走 NVMe） */
    if (g_nvme.ready) {
        int prc = sec_rd(0, 1, mbr);
        { char pdbg[96]; ksprintf(pdbg, "[KERNEL/FS] nvprobe rd=%d sig=%02X%02X b0=%02X\n",
                                  prc, mbr[510], mbr[511], mbr[0]); out_file_str(pdbg); }
        if (prc == 0 && mbr[510] == 0x55 && mbr[511] == 0xAA) {
            u32 pl = *(u32*)(mbr + 454);
            if (pl && sec_rd(pl, 1, buf) == 0
                && buf[3]=='E'&&buf[4]=='X'&&buf[5]=='F'&&buf[6]=='A'&&buf[7]=='T'
                && buf[510]==0x55 && buf[511]==0xAA) {
                g_ch = 0; g_dv = 0; g_part_lba = pl; found = 1;
            }
        }
        if (!found) return -8;      /* NVMe 上没 exFAT 卷（空白盘） */
    }
    if (g_dev_forced) {
        /* UEFI 给的位置：只验这一个点（读分区引导扇区确认 EXFAT 签名）*/
        if (ide_read_sector(g_ch, g_dv, g_part_lba, 1, buf) == 0
            && buf[3]=='E'&&buf[4]=='X'&&buf[5]=='F'&&buf[6]=='A'&&buf[7]=='T'
            && buf[510]==0x55 && buf[511]==0xAA) found = 1;
        else return -7;   /* 位置已知但验不过——不再瞎试其它盘位（错盘比慢更糟）*/
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
    if (!found) return -8;   /* 4 个盘位都读不到 ExFAT */
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
u32 get_root_dir_cluster(char module[]) {
    out_file_str("[EXFAT/API CALL] API call from ");
    out_file_str(module);
    out_file_str("\n");
    return g_boot.root_dir_cluster;
}
