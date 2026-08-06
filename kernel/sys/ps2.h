/* sys/ps2.h — PS/2: FIFO + mouse + keyboard */
#pragma once
#include "../boot/types.h"
#include "../drivers/io.h"

#define FS 32
struct Fifo { u8 buf[FS]; int r,w,n; };
extern Fifo mfifo, kfifo;

void fp(Fifo *f, u8 d) { if(f->n<FS){f->buf[f->w++]=d;f->w%=FS;f->n++;} }
int fg(Fifo *f, u8 *d) { if(!f->n)return 0;*d=f->buf[f->r++];f->r%=FS;f->n--;return 1; }

void mwait() { while(in8(0x64)&0x02){} }
void mouse_enable() {
    mwait(); out8(0x64,0xD4); mwait(); out8(0x60,0xF4);
    /* verify the mouse ACKs 0xFA — drain it here so md() starts clean */
    int t=0; while(!(in8(0x64)&0x01) && t<2000) t++;   /* 等 ACK 减到 2k 次——QEMU PS/2 设备未响应时原 30 万次 in8 卡死(freeze) */
    if(in8(0x64)&0x01) in8(0x60);   /* consume 0xFA ACK */
}

void init_keyboard() {
    /* 8042 config byte 0x67 = original 0x47 + translation bit5!
       bit0 kbd IRQ, bit1 mouse IRQ, bit2 sysflag,
       bit3=0 kbd clock ENABLED (0x3F had bit3=1 = clock DISABLED → no IRQ1!),
       bit4=0 mouse clock ENABLED, bit5=1 Set2→Set1 translation (QEMU translate_table),
       bit6 reserved (original keeps it 1). */
    while(in8(0x64)&0x02){} out8(0x64,0x60); while(in8(0x64)&0x02){} out8(0x60,0x67);
    /* 8042 command 0xAE: enable keyboard interface (UEFI may have disabled it) */
    while(in8(0x64)&0x02){} out8(0x64,0xAE);
    /* keyboard command 0xF4: enable scanning — without it the keyboard
       never sends data → IRQ1 never fires! (UEFI leaves it disabled) */
    while(in8(0x64)&0x02){} out8(0x60,0xF4);
    /* MUST consume the 0xFA ACK — wait for it with a generous timeout.
       If we never get 0xFA the keyboard is NOT enabled → IRQ1 will never fire! */
    int got=0;
    for(int i=0;i<200000;i++){
        if(in8(0x64)&0x01){ u8 b=in8(0x60); if(b==0xFA){ got=1; break; } }
    }
    /* report result raw on COM1 */
    out8(0x3F8,'K'); out8(0x3F8,':');
    out8(0x3F8, got ? '1' : '0'); out8(0x3F8,'\n');
}

struct MP { u8 b[3], phase, btn; int x,y; };
int md(MP *m, u8 d) {
    if(m->phase==0&&d==0xFA){m->phase=1;return 0;}
    /* yuanbao fix: first byte of a PS/2 mouse packet MUST have the marker bits
       (0xC8 mask → 0x08). Any other byte = stream desync → skip it, don't
       accumulate a wrong packet (that caused ±255 delta jumps). */
    if(m->phase==1){ if((d&0xC8)!=0x08) return 0; m->b[0]=d; m->phase=2; return 0; }
    if(m->phase==2){ m->b[1]=d; m->phase=3; return 0; }
    if(m->phase==3){ m->b[2]=d; m->phase=1; m->btn=m->b[0]&0x07; m->x=m->b[1]; m->y=m->b[2];
        if(m->b[0]&0x10)m->x|=0xFFFFFF00;if(m->b[0]&0x20)m->y|=0xFFFFFF00;m->y=-m->y;return 1;}
    return -1;
}
