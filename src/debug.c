#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <stdint.h>
#include <sys/prx.h>
#include <lv2/sysfs.h>
#include <ppu-lv2.h>
#include <lv2/syscalls.h>
#include "debug.h"

/* VSH notification function (undocumented) */
#define VSH_NOTIFY_ADDR 0x80000000004C0C30  /* Typical VSH notify address */

static int debug_level = DEBUG_LEVEL_INFO;
static int debug_initialized = 0;

/* VSH notification function signature */
typedef int (*vsh_notify_t)(int mode, const char *text);

void debug_init(int level) {
    debug_level = level;
    debug_initialized = 1;
    
    /* Log initialization */
    sysFSStat st;
    if (sysFsStat("/dev_hdd0/tmp", &st) == 0) {
        int fd;
        if (sysFsOpen("/dev_hdd0/tmp/cd_plugin_debug.log", 
                      SYS_O_WRONLY | SYS_O_CREAT | SYS_O_APPEND, &fd, NULL, 0) == 0) {
            const char *msg = "[DEBUG] Debug system initialized\n";
            uint64_t written;
            sysFsWrite(fd, msg, strlen(msg), &written);
            sysFsClose(fd);
        }
    }
    
    if (debug_level >= DEBUG_LEVEL_INFO) {
        debug_notify("CD Info Plugin: Debug enabled (level %d)", level);
    }
}

int debug_get_level(void) {
    return debug_level;
}

void debug_set_level(int level) {
    debug_level = level;
}

void debug_notify(const char *format, ...) {
    if (!debug_initialized || debug_level == DEBUG_LEVEL_NONE) return;
    
    char text[256];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    
    /* Try to call VSH notification */
    /* Note: This is firmware-specific and may need adjustment */
    #ifdef VSH_NOTIFY_ADDR
    vsh_notify_t vsh_notify = (vsh_notify_t)(uintptr_t)VSH_NOTIFY_ADDR;
    if (vsh_notify) {
        vsh_notify(0, text);
    }
    #endif
    
    /* Also log to file */
    debug_log(DEBUG_LEVEL_INFO, "NOTIFY: %s", text);
}

void debug_notify_icon(const char *text, int icon_type) {
    if (!debug_initialized || debug_level == DEBUG_LEVEL_NONE) return;
    
    /* Icon type affects the notification appearance */
    /* 0=info, 1=caution, 2=friend, etc. */
    (void)icon_type;  /* Use in future implementation */
    
    debug_notify("%s", text);
}

void debug_log(int level, const char *format, ...) {
    if (!debug_initialized || level > debug_level) return;
    
    char text[512];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    
    /* Write to debug log */
    int fd;
    if (sysFsOpen("/dev_hdd0/tmp/cd_plugin_debug.log",
                  SYS_O_WRONLY | SYS_O_CREAT | SYS_O_APPEND, &fd, NULL, 0) == 0) {
        char log_line[600];
        const char *level_str = (level == DEBUG_LEVEL_ERROR) ? "ERROR" :
                               (level == DEBUG_LEVEL_INFO) ? "INFO" : "VERBOSE";
        snprintf(log_line, sizeof(log_line), "[%s] %s\n", level_str, text);
        uint64_t written;
        sysFsWrite(fd, log_line, strlen(log_line), &written);
        sysFsClose(fd);
    }
}
