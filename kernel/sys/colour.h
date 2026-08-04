/* sys/colour.h — RGB macros for QEMU GOP PixelRedGreenBlue format!
   (user confirmed: framebuffer is RGB, NOT BGR — byte0=Red, byte1=Green, byte2=Blue)
   RGB(255,0,0) = 0x000000FF = red, RGB(255,255,0) = 0x0000FFFF = yellow */
#pragma once

#define RGB(r,g,b) (((b)<<16)|((g)<<8)|(r))
#define RGBA(r,g,b,a) (((a)<<24)|((b)<<16)|((g)<<8)|(r))
