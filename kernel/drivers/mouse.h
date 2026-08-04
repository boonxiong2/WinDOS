/* mouse.h — polled PS/2 mouse */
#pragma once
#include "../boot/types.h"
#include "io.h"

#define PORT_KEYDAT 0x0060
#define PORT_KEYSTA 0x0064
#define PORT_KEYCMD 0x0064
#define KBC_OBF 0x01
#define KBC_IBF 0x02

static inline void kbc_wait_in() { while (!(in8(PORT_KEYSTA) & KBC_OBF)); }
static inline void kbc_wait_out() { while (in8(PORT_KEYSTA) & KBC_IBF); }

static inline void mouse_init() {
    kbc_wait_out(); out8(PORT_KEYCMD, 0xA8);
    kbc_wait_out(); out8(PORT_KEYCMD, 0x20); kbc_wait_in();
    u8 cfg = in8(PORT_KEYDAT);
    kbc_wait_out(); out8(PORT_KEYCMD, 0x60); kbc_wait_out();
    out8(PORT_KEYDAT, (cfg | 2) & 0x3F);
    kbc_wait_out(); out8(PORT_KEYCMD, 0xD4); kbc_wait_out();
    out8(PORT_KEYDAT, 0xF4);
    while (in8(PORT_KEYSTA) & KBC_OBF) { in8(PORT_KEYDAT); }
}

struct MousePacket { int x, y; };

static inline int mouse_poll(MousePacket *pkt) {
    static u8 phase = 0, buf[3];
    if (!(in8(PORT_KEYSTA) & KBC_OBF)) return 0;
    u8 d = in8(PORT_KEYDAT);
    if (phase == 0) { if (d == 0xFA) phase = 1; return 0; }
    buf[phase++ - 1] = d;
    if (phase == 4) {
        phase = 1;
        pkt->x = buf[1]; pkt->y = buf[2];
        if (buf[0] & 0x10) pkt->x |= 0xFFFFFF00;
        if (buf[0] & 0x20) pkt->y |= 0xFFFFFF00;
        pkt->y = -pkt->y;
        return 1;
    }
    return 0;
}
