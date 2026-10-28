#pragma once

/*
 * 轻量日志：printf 风格，追加写入 /dev_hdd0/tmp/cd_plugin.log。
 * 首次调用自动创建目录，无需初始化。实现见 src/main.c。
 */
void plugin_log(const char *fmt, ...);
