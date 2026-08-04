/* sys/stdio.h — minimal sprintf for kernel */
#pragma once
#include "../boot/types.h"
#include "stdarg.h"

static void pkd(char **p, int v) {
    if(v<0){*(*p)++='-';v=-v;}
    int t=v,d=1; while(t>=10){t/=10;d*=10;}
    while(d){*(*p)++='0'+v/d%10;d/=10;}
}
static void pkx(char **p, unsigned v, int w) {
    for(int i=w-1;i>=0;i--){int n=(v>>(i*4))&0xF;*(*p)++=n<10?'0'+n:'A'+n-10;}
}

static int ksprintf(char *buf, const char *fmt, ...) {
    va_list va; va_start(va, fmt);
    char *p = buf;
    for (int i = 0; fmt[i]; i++) {
        if (fmt[i] == '%') {
            i++;
            if(fmt[i]=='s'){ const char *s=va_arg(va,const char*); while(*s)*p++=*s++; }
            else if(fmt[i]=='d'){ int v=va_arg(va,int); pkd(&p,v); }
            else if(fmt[i]=='x'){ int v=va_arg(va,int); pkx(&p,v,8); }
            else if(fmt[i]=='X'){ int v=va_arg(va,int); pkx(&p,v,4); }
            else if(fmt[i]=='0' && fmt[i+1]=='2' && fmt[i+2]=='X'){
                int v=va_arg(va,int); pkx(&p,v,2); i+=2;   /* %02X → 2 hex digits */
            }
            else *p++=fmt[i];
        } else *p++ = fmt[i];
    }
    *p = 0;
    va_end(va);
    return p - buf;
}

#define sprintf ksprintf
