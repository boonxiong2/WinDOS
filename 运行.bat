@echo off
cd /d "%~dp0"
start "" "C:\Program Files\qemu\qemu-system-x86_64w.exe" -m 512 -bios "%~dp0..\OVMF.fd" -drive "file=fat:rw:%~dp0esp,format=raw,if=none,id=esp" -device "ide-hd,drive=esp,bootindex=1"
