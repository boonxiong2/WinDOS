/* sys/panic.h — kernel panic */
#pragma once
#include "../security/dead_screen.h"

static __attribute__((noreturn)) void panic(const char *msg) {
    ds(msg);
    __builtin_unreachable();
}
