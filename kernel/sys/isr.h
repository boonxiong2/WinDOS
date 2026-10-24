/* sys/isr.h — 中断服务程序（ISR）
   鼠标处理在 isr2c（内核主循环鼠标分支原样复制）——登录界面和用户态都工作
   isr20 = PIT 时钟   isr21 = 键盘   isr2c = 鼠标 */
#pragma once
#include "ps2.h"
#include "../window/window.h"

/* shared mouse/window state (defined in kernel.cpp) */
extern SHTCTL *g_shtctl;
extern MP g_m; extern int g_mx, g_my;
extern struct SHEET *g_cur_sht, *g_drag_sht, *g_login_sht;
extern int g_mmx, g_mmy;

/* PIT 时钟中断（IRQ0，每 10ms 一次）：
   g_ticks++ 计时（主循环心跳/登录超时用）
   发 EOI(0x60) 告诉 PIC 中断处理完
   注意：这里不再读鼠标——鼠标归 isr2c 独占（之前 isr20 也读 0x60
   和 isr2c 抢数据导致光标乱飞） */
extern "C" void isr20_handler() { static int c20=0; if(!c20){out8(0x3F8,'2');out8(0x3F8,'0');out8(0x3F8,':');out8(0x3F8,'\n');c20=1;} extern volatile u32 g_ticks; g_ticks++; out8(0x20,0x60); }
volatile u32 _isr21_fires = 0;
/* 键盘中断（IRQ1）：
   读 0x64 状态：bit5=0 才是键盘数据（bit5=1 是鼠标数据——留给 isr2c）
   键盘扫描码塞进 kfifo（主循环取出来处理）
   每 4096 次打一次串口计数（调试） */
extern "C" void isr21_handler() {
    _isr21_fires++;
    if((_isr21_fires & 0xFFF) == 0){
        /* raw serial: "I<count-hex> " via COM1 0x3F8 */
        u32 v = _isr21_fires;
        out8(0x3F8,'I'); out8(0x3F8,':');
        for(int sh=28; sh>=0; sh-=4){ int n=(v>>sh)&0xF; out8(0x3F8, n<10?'0'+n:'A'+n-10); }
        out8(0x3F8,' '); out8(0x3F8,'\n');
    }
    out8(0x20,0x61);   /* SPECIAL EOI for IRQ1 — original haribote! (0x60 can clear the
                          wrong in-service bit when PIT is also pending → IRQ1 sticks!) */
    u8 s=in8(0x64);
    if((s&0x01) && !(s&0x20)) fp(&kfifo,in8(0x60));  // kbd data only (bit5=0 → not mouse)
}
/* 鼠标中断（IRQ12）：这里处理鼠标的完整逻辑（点击/拖拽/关闭/光标）
   先发特殊 EOI（0xA0,0x64 + 0x20,0x62——原版 haribote 的做法，
   普通 EOI 会清错 in-service 位导致 IRQ 卡住）
   然后读 0x64：bit5=1 才是鼠标数据（bit5=0 是键盘——留给 isr21） */
/* ── 与 kernel.cpp 同步的 UI 常量（改一处就得两处一起改）──
   SHADOW    : 窗口阴影带宽（draw_win_shadow）
   UI_SCALE  : 窗口/字/光标的放大倍数（标题栏 24*UI_SCALE 高、关闭钮 16*UI_SCALE x 14*UI_SCALE）
   CUR_H     : 光标高 = 16 * UI_SCALE（鼠标边界钳位用） */
#ifndef SHADOW
#define SHADOW 16
#endif
#ifndef UI_SCALE
#define UI_SCALE 2
#endif
#ifndef CUR_H
#define CUR_H (16 * UI_SCALE)
#endif
extern "C" void taskbar_click(int x, int y);
extern "C" void win_closed(struct SHEET *s);
extern "C" void win_minimize(struct SHEET *s);   /* ⑦ 任务栏点击（定义在 kernel.cpp）*/
extern "C" void isr2c_handler() {
    static int c2c=0; if(!c2c){out8(0x3F8,'2');out8(0x3F8,'c');out8(0x3F8,':');out8(0x3F8,'\n');c2c=1;}
    out8(0xA0,0x64); out8(0x20,0x62);   /* 特殊 EOI（IRQ12+IRQ2）——原版 */
    u8 s=in8(0x64);
    if((s&0x01)&&(s&0x20)){
        u8 d=in8(0x60);
        /* ── kernel main-loop mouse branch, copied VERBATIM (globals) ── */
        if(md(&g_m,d)!=0){
            /* click detection FIRST (old coords — original order) */
            if((g_m.btn&0x01)!=0){ /* left pressed */
                if(g_mmx<0){ /* normal mode */
                    /* ⑦ 底部 40px 是任务栏：交给 taskbar_click，不参与图层命中 */
                    if(g_my >= (int)g_shtctl->ysize - 40){ taskbar_click(g_mx, g_my); }
                    else {
                    /* ORIGINAL: j > 0 — skip sheets[0] = sht_back! */
                    for(int j=g_shtctl->top;j>0;j--){
                        struct SHEET *sht=g_shtctl->sheets[j];
                        if(sht==g_cur_sht) continue;
                        int x=g_mx-sht->vx0, y=g_my-sht->vy0;
                        if(0<=x&&x<sht->bxsize&&0<=y&&y<sht->bysize){
                            u32 pix=sht->buf[y*sht->bxsize+x];
                            if(pix!=(u32)0x00FF00FF){
                                if(j<g_shtctl->top-1) sheet_updown(sht, g_shtctl->top-1);
                                sheet_updown(g_cur_sht, g_shtctl->top);
                                /* ★ 判定顺序：关闭 → 最小化 → 标题栏拖动（按钮必须在拖动之前，
                                   否则点按钮会变成拖窗口——之前 minimize 就是这么失效/崩的） */
                                int in_close = (sht!=g_login_sht
                                    && sht->bxsize-SHADOW-21*UI_SCALE<=x && x<sht->bxsize-SHADOW-5*UI_SCALE
                                    && 5*UI_SCALE+SHADOW<=y && y<19*UI_SCALE+SHADOW);
                                int in_min = (sht->bxsize-SHADOW-37*UI_SCALE<=x && x<sht->bxsize-SHADOW-21*UI_SCALE
                                    && 5*UI_SCALE+SHADOW<=y && y<19*UI_SCALE+SHADOW);
                                int in_title = (3*UI_SCALE<=x && x<sht->bxsize-3*UI_SCALE
                                    && 3*UI_SCALE+SHADOW<=y && y<21*UI_SCALE+SHADOW);
                                if(in_close){
                                    win_closed(sht);              /* 关闭 = 销毁（含任务栏槽）*/
                                    g_mmx=-1; g_drag_sht=0;
                                } else if(in_min){
                                    win_minimize(sht);            /* 最小化 = 隐藏（对象保留）*/
                                    g_mmx=-1; g_drag_sht=0;
                                } else if(in_title){
                                    g_mmx=g_mx; g_mmy=g_my; g_drag_sht=sht;
                                }
                                break;
                            }
                        }
                    }
                    }   /* end else: 任务栏区域之外 */
                }else{ /* drag mode — Windows-style free movement */
                    int x=g_mx-g_mmx, y=g_my-g_mmy;
                    if(x<-1||x>1||y<-1||y>1){
                        if(g_drag_sht) sheet_slide(g_drag_sht, g_drag_sht->vx0+x, g_drag_sht->vy0+y);
                        g_mmx=g_mx; g_mmy=g_my;
                    }
                }
            } else {
                g_mmx=-1; g_drag_sht=0; /* no button → normal mode */
            }
            /* THEN update cursor position (original order) */
            g_mx+=g_m.x; g_my+=g_m.y;
            if(g_mx<0)g_mx=0;
            if(g_my<0)g_my=0;
            if(g_mx>(int)g_shtctl->xsize-CUR_H)g_mx=g_shtctl->xsize-1;
            if(g_my>(int)g_shtctl->ysize-CUR_H)g_my=g_shtctl->ysize-1;
            if(g_cur_sht) sheet_slide(g_cur_sht, g_mx, g_my);
        }
    }
}
extern "C" void isr_default_handler() {}
extern "C" void isr20(), isr21(), isr2c(), isr_default();
