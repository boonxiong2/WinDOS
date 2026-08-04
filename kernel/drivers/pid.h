/* pid.h — simple PID allocator */
#pragma once
#include "../boot/types.h"

typedef u32 pid_t;

static pid_t _next_pid = 1;

static inline pid_t pid_alloc() { return _next_pid++; }
static inline pid_t pid_current() { return _next_pid - 1; }
