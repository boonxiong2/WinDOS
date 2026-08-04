@echo off
cd /d %~dp0
echo Starting QEMU...
"C:\Program Files\qemu\qemu-system-x86_64w.exe" -m 512 -bios "..\OVMF.fd" -drive "file=fat:rw:esp,format=raw,if=none,id=esp" -device "ide-hd,drive=esp,bootindex=1" 2>nul
if %errorlevel% neq 0 (
    echo QEMU not found at default path.
    echo Edit run.bat and change the path to qemu-system-x86_64w.exe
    pause
)
