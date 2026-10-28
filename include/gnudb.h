#pragma once
#include <stdint.h>

#define MAX_TRACKS    99
#define MAX_STR_LEN   128
#define MAX_TRACK_LEN 64
#define CACHE_SIZE    8

/* 存储一张CD的三个核心字段 */
typedef struct {
    char     artist[MAX_STR_LEN];
    char     album[MAX_STR_LEN];
    char     tracks[MAX_TRACKS][MAX_TRACK_LEN];
    uint8_t  track_count;
    uint32_t discid;
    uint8_t  valid;      /* 0=空槽 1=有效数据 */
    uint8_t  not_found;  /* 1=gnudb 已确认查不到（负缓存） */
} CDInfo;

/*
 * 初始化缓存，清空所有槽位
 * 在 main.c 的 start 入口里调用一次
 */
void gnudb_cache_init(void);

/*
 * 查缓存，找到返回指针，未命中返回NULL
 */
CDInfo *gnudb_cache_get(uint32_t discid);

/*
 * 写入缓存，LRU淘汰最久未用的槽位
 */
void gnudb_cache_put(uint32_t discid, const CDInfo *info);

/*
 * 通过 CDDB disc ID + TOC 查询 gnudb（纯 HTTP，两次 GET：query + read）
 * 成功把结果写入 out 并返回 0，失败返回 -1
 * discid_str: 8位hex CDDB discid，例如 "a50d4a0c"
 * toc_query:  "FIRST+LAST+LEADOUT+OFFSET1+OFFSET2+..."（帧，含 +150）
 *             cddb query 命令的偏移与秒数都从这里还原，不能为空
 */
int gnudb_query(const char *discid_str, const char *toc_query, CDInfo *out);

/*
 * 解析 gnudb cddb read 返回的 CDDB 文本记录，写入 out。
 * 纯字符串逻辑、不依赖 PS3 头文件，主机侧单元测试直接编译
 * src/gnudb_parse.c 即可验证。
 * 成功返回 0（out->valid 置位），失败返回 -1。
 */
int gnudb_parse_record(const char *record, CDInfo *out);
