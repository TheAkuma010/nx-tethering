#include <stdio.h>
#include <stdarg.h>
#include <switch.h>

static FILE* g_log_file;

void log_init(void) {
    fsInitialize();
    fsFsMountSdcard();
    g_log_file = fopen("sdmc:/usbnet.log", "w");
}

void log_print(const char* fmt, ...) {
    if (!g_log_file) return;
    va_list args;
    va_start(args, fmt);
    vfprintf(g_log_file, fmt, args);
    fflush(g_log_file);
    va_end(args);
}

void log_exit(void) {
    if (g_log_file) fclose(g_log_file);
}
