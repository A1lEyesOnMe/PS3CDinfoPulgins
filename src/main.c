#include <stdio.h>
#include <string.h>
#include <sys/prx.h>
#include <lv2/sysfs.h>
#include <ppu-types.h>
#include "hook_net.h"
#include "gnudb.h"

/* ------------------------------------------------------------------ */
/* PRX入口函数声明                                                       */
/* ------------------------------------------------------------------ */

s32 _ps3_cd_info_start(size_t args, void *argp);
s32 _ps3_cd_info_stop(size_t args, void *argp);

/* ------------------------------------------------------------------ */
/* 日志函数                                                              */
/* ------------------------------------------------------------------ */

static void plugin_log(const char *msg) {
    s32 fd;
    u64 written;
    
    /* 使用正确的函数名：sysFsOpen, sysFsWrite, sysFsClose */
    if (sysFsOpen("/dev_hdd0/tmp/cd_plugin.log",
                  SYS_O_WRONLY | SYS_O_CREAT | SYS_O_APPEND,
                  &fd, NULL, 0) == 0) {
        sysFsWrite(fd, msg, strlen(msg), &written);
        sysFsWrite(fd, "\n", 1, &written);
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
