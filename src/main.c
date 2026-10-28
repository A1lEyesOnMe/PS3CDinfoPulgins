#include <stdio.h>
#include <string.h>
#include <stdarg.h>
#include <sys/prx.h>
#include <lv2/sysfs.h>
#include <ppu-types.h>
#include "hook_net.h"
#include "gnudb.h"
#include "log.h"

/* ------------------------------------------------------------------ */
/* PRX入口函数声明                                                       */
/* ------------------------------------------------------------------ */

s32 _ps3_cd_info_start(size_t args, void *argp);
s32 _ps3_cd_info_stop(size_t args, void *argp);

/* ------------------------------------------------------------------ */
/* 日志函数                                                              */
/* ------------------------------------------------------------------ */

static int log_dir_ok = 0;

/*
 * 轻量日志：printf 风格，逐条 open/write/close 追加到日志文件。
 * 目录 /dev_hdd0/tmp 可能不存在，首次写入前用 sysFsMkdir 确保。
 */
void plugin_log(const char *fmt, ...) {
    s32 fd;
    u64 written;
    char buf[512];
    va_list ap;
    int n;

    if (!log_dir_ok) {
        sysFsMkdir("/dev_hdd0/tmp", 0777);
        log_dir_ok = 1;
    }

    va_start(ap, fmt);
    n = vsnprintf(buf, sizeof(buf) - 1, fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t)n >= sizeof(buf) - 1) n = sizeof(buf) - 1;
    buf[n++] = '\n';

    if (sysFsOpen("/dev_hdd0/tmp/cd_plugin.log",
                  SYS_O_WRONLY | SYS_O_CREAT | SYS_O_APPEND,
                  &fd, NULL, 0) == 0) {
        sysFsWrite(fd, buf, (u64)n, &written);
        sysFsClose(fd);
    }
}

/* ------------------------------------------------------------------ */
/* PRX启动函数                                                           */
/* ------------------------------------------------------------------ */

s32 _ps3_cd_info_start(size_t args, void *argp) {
    (void)args;
    (void)argp;

    plugin_log("PS3 CD Info Plugin starting...");

    /* 初始化缓存 */
    gnudb_cache_init();
    plugin_log("Cache initialized");

    /* 安装网络hook */
    if (hook_net_init() < 0) {
        plugin_log("ERROR: Failed to install network hook");
        return SYS_PRX_STOP_OK;
    }
    plugin_log("Network hook installed");

    plugin_log("Plugin started successfully");

    return SYS_PRX_RESIDENT;
}

/* ------------------------------------------------------------------ */
/* PRX停止函数                                                           */
/* ------------------------------------------------------------------ */

s32 _ps3_cd_info_stop(size_t args, void *argp) {
    (void)args;
    (void)argp;

    plugin_log("PS3 CD Info Plugin stopping...");

    hook_net_cleanup();
    plugin_log("Network hook removed");

    plugin_log("Plugin stopped");
    return SYS_PRX_STOP_OK;
}
