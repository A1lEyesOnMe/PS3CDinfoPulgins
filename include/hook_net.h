#pragma once
#include <stddef.h>

/*
 * 初始化网络hook
 * 挂载 sys_net_recv 钩子，拦截CDDB响应
 *
 * 在 main.c 的 prx_start 里调用
 * 成功返回 0，失败返回 -1
 */
int hook_net_init(void);

/*
 * 清理hook，恢复原始函数
 * 在 prx_stop 里调用
 */
void hook_net_cleanup(void);

/*
 * 从CDDB请求文本中提取 disc ID（8位hex）并还原 MusicBrainz fuzzy TOC：
 * "FIRST+LAST+LEADOUT+OFFSET1+OFFSET2+..."
 * 成功返回 0，失败返回 -1。
 * （实现在 src/cddb_query.c，主机侧可直接单测）
 */
int parse_cddb_query(const char *request, char *discid_out,
                     size_t discid_out_size,
                     char *toc_out, size_t toc_out_size);
