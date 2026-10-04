# WinDOS — Future Roadmap

`[+]` 完成　`[-]` 部分完成　`[ ]` 未开始

## Core System
- [+] 动态内存管理（堆分配器：slab/buddy）
- [+] 多任务调度（轮转 + 优先级）
- [ ] 进程间通信（IPC）
- [-] 分级登录特权：
  - 登录界面/登录管理器 = Ring 0 系统组件 ✅
  - `Sysdebug` 用户登录 → Ring 1 会话 ⚠️（Ring 1 已放弃——Intel 传闻删除 ring1/2，现代 OS 只用 0/3；改用软分级=内核记等级控制 syscall 开放）
  - 其他用户登录 → Ring 3 会话 ✅

## Storage
- [+] 磁盘 I/O（ATA PIO）：真盘**读 + 写**已通 ✅
  - 读：引导卷分区引导扇区 → `oem="MSWIN4.1"` + `aa55=55AA`（日志 `ATAread … ret=0`）
  - 写：签名盘 `logdisk` 写扇区 + 读回校验 = `WRITE_OK`
  - ⚠️ 旧结论"QEMU IDE PIO 已知损坏 / DRQ 永不置位"是**误判**——真凶是 `drivers/io.h` 的 `in8` 没有 `volatile`（详见 Bug Reports）
- [-] ExFAT：**读**可用（Windows 格式化的 exFAT 测试盘实测 `[FS] exFAT init OK`）；**写**还不完整：
  - 待补字段：0x85 项 `SecondaryCount` / `SetChecksum` / 属性位置（现错写在 byte 2-3，属性该在 byte 4-5）、0xC0 项 `NameHash` / `NoFatChain`
  - 待补算法：exFAT 的 16 位"旋转右移 1 + 加"校验和（**不是 CRC32**），覆盖 (SecondaryCount+1)*32 字节、跳过自身 2 字节
  - 待修：目录簇号写死 `2`（测试盘根簇=**5**，照原样会把分配位图当目录写坏）、`u8 bm[512]` 对 4KB 簇会**栈溢出**、只支持单簇（≤4KB）、只写根目录、位图只写 1 扇区、名字只取低字节（中文名乱）
  - 测试台：`run_exfat_test.bat` + `exfat_test.vhd`(MBR) / `exfat_gpt_test.vhd`(GPT，均 16MB/4KB簇/卷标 WD*)；判据 = 关机后 `chkdsk X: /f` 干净 + 资源管理器能看到文件
  - 目标：**用户自己实现**
- [-] **exFAT 写路径的三条地基**（2026-10 手写时理清，别再重新推导）
  - **扇区 512B 是规范常量**（ATA 硬规定，可写死）；**每簇扇区数是从盘读的**（BPB `0x6D` 的 2 次幂 → `g_spc`，本盘 8）→ 凡按"扇区数"推进/循环的地方**必须用 `g_spc`，不能写死 8**
  - **卷参数 vs 规范**：`bp[0x60]` 是**根目录簇号**（本盘 = **5**，Windows 格式化的盘；我们自己 `mkmemdisk.py` 生成的镜像 = 2）；**簇号空间起点永远是 2** → `cluster_to_lba` 里减的**永远是 2**，与根目录在第几号簇无关
    - 盘上铁证（`exfat_test.vhd` 同号簇两种公式）：`-2` → LBA 408 → 首字节 `0x83` 卷标条目（根目录真身 `WDTEST`）；`-5` → LBA 384 → 首字节 `0x3F` + 一片 0（空地）
    - 写死的 `2` 该换成 `g_boot.root_dir_cluster`（三处：`exfat.cpp` 读目录 / 写回目录 / `kernel.cpp` 的 `exfat_list_dir`）
  - **缓冲必须 = 整簇**：位图那段 `u8 bm[512]` 装不下 4KB 簇 → `read_cluster(bm_cluster, bm)` **越界写 3584B 踩坏栈上邻居**（比位图写脏更危险），`ide_write_sector(..., 8, bm)` 也只写了 1/8 真数据 + 7/8 内存垃圾
    - 修法：`bm` 尺寸 = 整簇（本盘 4096，通用给 65536 或按 `g_bpc`）；写回传 `g_spc`（不是 8）
  - `ide_write_sector(ch,dv,lba,count,buf)` 内部**就是逐扇区循环**（`for s<count` + `lba++`），所以 `count` 语义是"连续 N 个扇区"——传 `g_spc` 即整簇写 ✓
- [ ] GPT 支持：内核目前只解析 MBR（`*(u32*)(mbr+454)` = 第一分区 LBA）→ GPT 盘实测 `[FS] exFAT init FAIL r=-2`
  - 现象：GPT 的 LBA0 是保护性 MBR（0xEE，指向 LBA1）→ 我们读 LBA1 拿到的是 GPT 头（无 EXFAT 签名）→ 安全失败（不写坏）
  - 方案 A（推荐，已具备）：**不解析分区表**，直接用引导器 DevicePath 给的 `part_lba`（MBR/GPT 通吃）
  - 方案 B（练习）：LBA1 查 `"EFI PART"` → 条目数组 `0x48/0x50/0x54` → 取 `StartingLBA@0x20`；⚠️ GUID 混合端序、写 GPT 要重算 header/数组 CRC32（`0x10`/`0x58`）
- [ ] FAT32 读+写（引导卷就是 FAT；若日志要"拔盘在 Windows 里能看"，FAT32 比 exFAT 少一档复杂度：无校验和、无名字哈希）
- [ ] 日志落盘（两条路：① 裸扇区 appender 写签名盘——最小、已验证可写；② exFAT 写文件——要给 Windows 看时才需要）
- [ ] NVMe 驱动（`BAR0=FFFFFFFF` 的两个真因已查明：① `io.h` 非 volatile 污染 PCI 读（已修）② 只取低 32 位 BAR + `vid==0x1B36` 硬编码。PCI 读现已正常 → 值得重试一版 64 位 BAR）
- [ ] 虚拟文件系统（VFS）层
- [ ] RAM disk

## Devices / USB
- [ ] xHCI（USB）驱动：**目前完全没有 USB 栈**
  - 启动时就插着 U 盘：能进内核（固件读的），但**内核自己读不了它**（USB 在 xHCI 后面，ATA PIO 够不到）
  - 运行中插入：**完全无感**（无枚举/无热插拔/无中断；好在也不会崩——`g_ch/g_dv` 在启动时定死）
  - 真机上更致命：多数笔记本**没有 PS/2**，USB 键鼠 = 唯一输入 → 没有 xHCI 就是"看不见也点不动"
- [ ] 低成本半步：引导器用 UEFI `Block I/O` 协议枚举所有块设备（含 USB），把"有几块盘 / 各是什么类型 / 什么位置"写进 BootInfo —— 内核能用则用，不能用至少能**报出来**
- [ ] 热插拔：需要驱动里做端口状态变化 + 重新枚举（AHCI 支持；xHCI 更复杂）

## Graphics
- [-] 双缓冲渲染（区域刷新——事件驱动 + isr2c 统一处理）
- [+] 窗口拖拽 ✅（isr2c 中断内直接处理——Windows 式自由出屏）
- [-] 窗口关闭按钮（✅ 已做 closebtn——最小化/最大化未做）
- [+] **UI 缩放**（`UI_SCALE=2` @FHD：字形/标题栏/关闭钮/鼠标光标全部 2×；登录窗按内容裁剪 560×150，不整窗等比撑开）
- [+] 2D 基元带裁剪（`sys/draw.h`：`fill_rect_c`/`blit_c`/`put_str_cs` + **哨兵自检** `[DRAW] selftest PASS`）
- [-] 还停在 1× 的部分：**任务栏（40px）+ 右下时钟**（一起上 `UI_SCALE`：40→80px、时钟字 16×32、重绘矩形同步）
- [ ] 真彩色图标

## Input
- [-] 键盘输入（PS/2 Set2 扫描码表已有——完整布局待补）
- [ ] 键盘组合键系统
- [ ] USB HID 驱动

## System
- [+] 异常处理（exc_handler + 死屏已有）
- [-] 系统调用（syscall_entry_asm → dispatch——SYS_WRITE 已通 + 指针校验）
- [-] 用户态/内核态分离：
  - Ring 3 ✅（sysret 降权 + syscall + 中断进出）
  - IOPL=0 + UMIP（CPUID 门控）✅——防 CIH 提权
  - syscall 指针校验 ✅（用户区限制）
- [ ] 页表 US 位收紧（现在 0…`_bss_end` 全翻 `US=1`，**内核页也在内** → Ring3 能读内核内存）
- [ ] IOMMU / VT-d（防 PCIe 设备的 DMA 攻击——雷电 / 热插拔显卡能直接读写内存）
- [ ] 可加载驱动模块

## 登录 / 账户
- [-] 登录界面（Ring0 组件、不可关闭）✅；用户名 → Ring3 ✅
- [ ] **密码框**（现在先留结构，以后加密码 = 填数据，不是改排版）
  - 两行输入（Username / Password）+ **Tab 切焦点** + Enter 提交
  - 密码回显 `*`；⚠️ **密码绝不能进串口日志**（现在 `[LOGIN] user='…'` 会把输入原样打出来，密码字段必须绕开）
  - 存储来源待定：① 引导器读 ESP 上的 `users.cfg` 塞进 BootInfo（现成能力）② 内核自己读 FAT（还没驱动）③ 先硬编码（demo）
  - 比对：存 hash + 盐、恒定时间；**定位是"防误入"，不是"防提权"**（要诚实标注）
  - ⚠️ 前提：US 位现在翻到 `_bss_end`（含内核页）→ 见 System 一节，真要安全登录得先收紧

## Applications
- [ ] 命令行终端（console）
- [ ] 文本编辑器
- [ ] 文件管理器
- [ ] 扫雷游戏（ClickedButton API 已设计，待移植）



## Bug Reports
- **【已修】端口 I/O 被编译器优化（= "QEMU IDE PIO 损坏"的真凶）**
  - 现象①：轮询 DRQ 永远超时（循环只读了一次）；②：PCI 配置读 6 次返回同一个值 `02800007`；③：写 LBA 寄存器后读回不对
  - 根因：`drivers/io.h` 的 `in8` 带输出操作数却**没有 `volatile`** → clang -O2 当纯函数做 CSE / 提出循环
    （`out8` 无输出操作数、按 GCC 规则隐含 volatile，才侥幸活下来）
  - 修法：`in8/in16/inl/out8/out16/outl` 全部 `volatile` + `"memory"` clobber
  - 判据：`ata port chk lba_lo reg: 5A->5A`、`ATAread … ret=0`（读到真 FAT 引导扇区）、`logdisk … WRITE_OK`
  - 同一天修掉的同类问题：DevicePath Messaging 子类型表写错（`0x12=SATA`/`0x17=NVMe`，我们写成 `0x10/0x12`
    → SATA 盘误报成 NVMe，q35 实测修后 `ctrl kind=0x2`）；窗口标题栏/关闭钮命中判定漏乘 `UI_SCALE`

- **Backspace 黑影二次出现（login 叠在 WinDOS 窗口）**
  - 登录窗口（400x200 中央）叠在 WinDOS 窗口（200x120）上——重叠区两窗口半透明阴影叠加（合成减——每层加深）变黑影
  - Backspace 输入框清空刷新时阴影区重画 → 黑影二次出现
  - 待修：阴影叠加/刷新联动（窗口重叠时的阴影处理）

- **【已修】登录后进不去 Ring3（jump_user 后卡死/蓝屏）**
  - 真凶：`kernel/sys/gdt.h` 里 2 个字面量少写 2 位十六进制（`0x00CFF20000FFFF` / `0x00CFB20000FFFF`）
    → 按描述符格式解码成 base=0x00F20000、access=0xCF 的**系统段**（S=0, type=F）
    → SS=0x23 不是可写数据段，sysret/iret 一执行就 #GP
    （`isr.S` 注释里的 `iretq to Ring3 #GPs (SS user-seg load e=0x20)`：错误码 0x20 正是选择子 0x23 的索引部分——当时的"修复"是改成 sysretq 绕开）
  - 沿途 4 个雷（少清一个还是进不去/不稳）：
    1. `sheet_*` 图层操作不可重入（isr2c 与主流程并发改图层表）→ 3 处 sheet 段用 cli/sti 包住
    2. `back_buf` 声明 800x600 却按 1280x800 用 → 越界写 ~2.1MB，糊掉 cur_buf/kstack/login_wbuf/g_fb/tss/user_stack → 改 1920x1080
    3. 翻页表 US 位写死只翻前 8MB（4 行）→ 改按 `_bss_end` 自动翻（.bss 已 10.6MB，user_stack 落在 10.8MB）
    4. 崩溃处理自身不安全（① 信被踩坏的 g_fb/g_stride ② 无防重入）→ 加防重入 guard + 显存指针合法性检查，不可信时只打串口不画屏
  - 附带：引导器 `.bss` 预留 6MB → 16MB（.bss 涨到 ~10.3MB）
  - 本树额外一处（同类病）：syscall 指针校验写死 `0x800000`（8MB）→ 把合法用户指针全判越界
    （现象 `[KERNEL/USER_API/EXCPTION] Pointer DIDN'T in user mem area`）→ 同按 `_bss_end` 推导
  - 验证：登录输入 `hello` 回车 → `J3` → `HELLO FROM RING3`（syscall 从 Ring3 打进内核并打印）；
    `info registers`：`CPL=3`、`CS =002b DPL=3 CS64`、`SS =0023 DPL=3 DS [-WA]`；
    Ring3 下连续 15s+、每秒 ~37 次 PIT 中断全部返回无 #GP（无蓝屏、无重启）
