#pragma once

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
