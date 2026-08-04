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

