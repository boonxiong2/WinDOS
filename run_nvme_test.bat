@echo off
cd /d %~dp0
set QEMU=C:\Program Files\qemu\qemu-system-x86_64w.exe
echo === WinDOS NVMe test bench ===
echo Serial log: %~dp0windos_log.log
echo QEMU stderr : %~dp0qemu_err.log
echo.
"%QEMU%" -m 512 ^
  -bios C:/Users/Administrator/Desktop/STORAGE/OVMF.fd ^
  -drive file=fat:rw:C:/Users/Administrator/Desktop/STORAGE/tolset/WinDOS/esp,format=raw,if=none,id=esp ^
  -device ide-hd,drive=esp,bootindex=1 ^
  -drive file=disks/nvme64.img,format=raw,if=none,id=nvm ^
  -device nvme,serial=WINDBG01,drive=nvm ^
  -serial file:windos_log.log -no-reboot 2>qemu_err.log
echo.
echo === QEMU exited (errorlevel=%errorlevel%) ===
echo --- qemu_err.log ---
type qemu_err.log
echo ----------------------
echo Log: %~dp0windos_log.log
pause
