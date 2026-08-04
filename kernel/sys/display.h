/* sys/display.h — screen resolution globals */
#pragma once
#include "../boot/types.h"

static u32 g_hr, g_vr, g_stride;
static u32 *g_fb;

static inline void disp_init(u32 *fb, u32 hr, u32 vr, u32 st) {
    g_fb = fb; g_hr = hr; g_vr = vr; g_stride = st;
}
static inline u32 disp_w() { return g_hr; }
static inline u32 disp_h() { return g_vr; }
static inline u32 disp_stride() { return g_stride; }
