cmake_minimum_required(VERSION 4.2)
project(WinDOS)

set(CMAKE_CXX_STANDARD 14)

include_directories(kernel)
include_directories(kernel/boot)
include_directories(kernel/drivers)
include_directories(kernel/window)
include_directories(uefi_rust/kernel/boot)
include_directories(uefi_rust/kernel/drivers)
include_directories(uefi_rust/kernel/window)

add_executable(WinDOS
        kernel/boot/types.h
        kernel/drivers/font.h
        kernel/drivers/font_data.h
        kernel/drivers/io.h
        kernel/drivers/mouse.h
        kernel/drivers/serial.h
        kernel/window/cursor.h
        kernel/window/window.h
        kernel/kernel.cpp
        kernel/WinDOS.h
        uefi_rust/kernel/boot/types.h
        uefi_rust/kernel/drivers/io.h
        uefi_rust/kernel/drivers/mouse.h
        uefi_rust/kernel/drivers/serial.h
        uefi_rust/kernel/window/cursor.h
        uefi_rust/kernel/window/window.h
        uefi_rust/kernel/kernel.cpp
        uefi_rust/kernel_old/bootpack.c
        uefi_rust/kernel_old/bootpack.h
        uefi_rust/kernel_old/dsctbl.c
        uefi_rust/kernel_old/fifo.c
        uefi_rust/kernel_old/graphic.c
        uefi_rust/kernel_old/int.c
        uefi_rust/kernel_old/keyboard.c
        uefi_rust/kernel_old/mouse.c
        uefi_rust/kernel_old/mtask.c
        uefi_rust/kernel_old/sheet.c
        uefi_rust/kernel_old/timer.c)
