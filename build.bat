@echo off
cd /d %~dp0
echo === WinDOS UEFI Kernel ===
echo.

if "%1"=="clean" (
    del /q kernel\*.o kernel\kernel.elf kernel\kernel.kern 2>nul
    echo Cleaned.
    exit /b 0
)

if not exist kernel.ld (
    echo ERROR: kernel.ld not found
    pause & exit /b 1
)

echo Compiling kernel...
echo const char *build_date = __DATE__ " " __TIME__; > kernel\build_info.h
clang++ --target=x86_64-elf -ffreestanding -nostdlib -O2 -mno-red-zone -fno-exceptions -fno-rtti -fno-stack-protector -fno-builtin -I kernel -I kernel/boot -I kernel/window -I kernel/drivers -mgeneral-regs-only -c kernel/isr.S -o kernel/isr.o
if %errorlevel% neq 0 (echo ERROR: ISR assembly failed & pause & exit /b 1)
clang++ --target=x86_64-elf -ffreestanding -nostdlib -O2 -mno-red-zone -fno-exceptions -fno-rtti -fno-stack-protector -fno-builtin -I kernel -I kernel/boot -I kernel/window -I kernel/drivers -fPIC -mgeneral-regs-only -c kernel/kernel.cpp -o kernel/kernel.o
clang++ --target=x86_64-elf -ffreestanding -nostdlib -O2 -mno-red-zone -fno-exceptions -fno-rtti -fno-stack-protector -fno-builtin -I kernel -I kernel/boot -I kernel/window -I kernel/drivers -fPIC -mgeneral-regs-only -c kernel/user/hello.cpp -o kernel/hello.o
if %errorlevel% neq 0 (echo ERROR: kernel compile failed & pause & exit /b 1)
clang++ --target=x86_64-elf -ffreestanding -nostdlib -O2 -mno-red-zone -fno-exceptions -fno-rtti -fno-stack-protector -fno-builtin -I kernel -I kernel/boot -I kernel/window -I kernel/drivers -fPIC -mgeneral-regs-only -c kernel/fs/exfat.cpp -o kernel/exfat.o
if %errorlevel% neq 0 (echo ERROR: exfat compile failed & pause & exit /b 1)
clang++ --target=x86_64-elf -ffreestanding -nostdlib -O2 -mno-red-zone -fno-exceptions -fno-rtti -fno-stack-protector -fno-builtin -I kernel -I kernel/boot -I kernel/window -I kernel/drivers -fPIC -mgeneral-regs-only -c kernel/fs/memdisk.cpp -o kernel/memdisk.o
if %errorlevel% neq 0 (echo ERROR: memdisk compile failed & pause & exit /b 1)
echo Linking...
ld.lld -T kernel.ld kernel/isr.o kernel/kernel.o kernel/hello.o kernel/exfat.o kernel/memdisk.o -o kernel/kernel.elf
if %errorlevel% neq 0 (echo ERROR: link failed & pause & exit /b 1)
llvm-objcopy -O binary kernel/kernel.elf kernel/kernel.kern
echo Generating memdisk.img (ExFAT + KERNEL.BIN)...
python tools\mkmemdisk.py
if %errorlevel% neq 0 (echo ERROR: mkmemdisk failed - need Python 3 or image over 256KB & pause & exit /b 1)
echo Building bootloader...
cargo build --release --target x86_64-unknown-uefi
if %errorlevel% neq 0 (echo ERROR: cargo build failed & pause & exit /b 1)
xcopy /y target\x86_64-unknown-uefi\release\windos-boot.efi esp\EFI\BOOT\BOOTX64.EFI >nul
echo === BUILD OK ===
echo Run: run.bat
