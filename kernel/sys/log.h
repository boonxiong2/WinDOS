/* sys/log.h — kernel logging */
#pragma once
#include "serial.h"

// Levels: CRITICAL (system-blocking, KERNEL PANIC) / ERROR (non-fatal) / WARNING
static void log_msg(const char *level, const char *msg) {
    out_str("[KERNEL/");
    out_str(level);
    out_str("] ");
    out_str(msg);
    out_str("\n");
}

#define LOG_CRITICAL(msg) log_msg("CRITICAL", msg)
#define LOG_ERROR(msg)    log_msg("ERROR", msg)
#define LOG_WARNING(msg)  log_msg("WARNING", msg)
#define LOG_INFO(msg)     log_msg("INFO", msg)
