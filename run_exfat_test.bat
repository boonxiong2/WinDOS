@echo off
cd /d %~dp0
set QEMU=C:\Program Files\qemu\qemu-system-x86_64w.exe
echo === WinDOS exFAT test bench ===
echo Serial log: %~dp0windos_log.log
echo Close the QEMU window to flush the log, then open it.
echo.
"%QEMU%" -m 512 ^
  -bios C:/Users/Administrator/Desktop/STORAGE/OVMF.fd ^
  -drive file=fat:rw:C:/Users/Administrator/Desktop/STORAGE/tolset/WinDOS/esp,format=raw,if=none,id=esp ^
  -device ide-hd,drive=esp,bootindex=1 ^
  -drive file=disks/exfat_test.vhd,format=raw,if=none,id=xt ^
  -device ide-hd,drive=xt ^
  -serial file:windos_log.log -no-reboot
echo.
echo === QEMU exited. Log: %~dp0windos_log.log ===
pause
