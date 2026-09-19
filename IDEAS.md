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
- [-] ExFAT 文件系统（fs/exfat.cpp 已写：Entry Set/UTF-16/MBR 分区解析——**未验证**：QEMU 的 IDE PIO 模拟 DRQ 永不置位）
- [ ] NVMe 驱动（尝试中——QEMU nvme 已挂载，BAR0 探测未通（配置读 FFFFFFFF）——需页表映射 32GB + Admin/IO 队列 + PRP）
- [ ] 虚拟文件系统（VFS）层
- [ ] RAM disk

## Graphics
- [-] 双缓冲渲染（区域刷新——事件驱动 + isr2c 统一处理）
- [+] 窗口拖拽 ✅（isr2c 中断内直接处理——Windows 式自由出屏）
- [-] 窗口关闭按钮（✅ 已做 closebtn——最小化/最大化未做）
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
- [ ] 可加载驱动模块

## Applications
- [ ] 命令行终端（console）
- [ ] 文本编辑器
- [ ] 文件管理器
- [ ] 扫雷游戏（ClickedButton API 已设计，待移植）



## Bug Reports
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
