@echo off
REM exFAT 测试台：WinDOS 引导盘 + 一块 Windows 格式化的 exFAT 测试盘
REM 测试盘：C:\Users\Administrator\exfat_test.vhd（16MB, exFAT, 卷标 WDTEST）
REM   分区 LBA=128, 簇=4KB, 根目录簇=5, 簇数 3792（位图 474B）
REM   重建：diskpart -> create vdisk file="C:\Users\Administrator\exfat_test.vhd" maximum=16 type=fixed
REM          attach vdisk / create partition primary / format fs=exfat quick label=WDTEST / detach vdisk
cd /d %~dp0
set QEMU=C:\Program Files\qemu\qemu-system-x86_64w.exe
"%QEMU%" -m 512 ^
  -bios C:/Users/Administrator/Desktop/STORAGE/OVMF.fd ^
  -drive file=fat:rw:C:/Users/Administrator/Desktop/STORAGE/tolset/WinDOS/esp,format=raw,if=none,id=esp ^
  -device ide-hd,drive=esp,bootindex=1 ^
  -drive file=C:/Users/Administrator/exfat_test.vhd,format=raw,if=none,id=xt ^
  -device ide-hd,drive=xt ^
  -serial stdio -no-reboot
