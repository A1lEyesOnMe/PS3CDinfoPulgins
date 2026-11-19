#pragma once
#include <stdint.h>
#include "amg_response.h"

/* Use AMG_MAX_TRACKS from amg_response.h, keep MAX_TRACKS for backward compatibility */
#define MAX_TRACKS    AMG_MAX_TRACKS
#define MAX_STR_LEN   128
#define MAX_TRACK_LEN 64
#define CACHE_SIZE    8

/* 存储一张CD的三个核心字段 */
typedef struct {
    char     artist[MAX_STR_LEN];
    char     album[MAX_STR_LEN];
    char     genre[MAX_STR_LEN];      /* 流派，MusicBrainz可获取 */
    char     tracks[MAX_TRACKS][MAX_TRACK_LEN];
    uint8_t  track_count;
    uint32_t discid;
    uint8_t  valid;  /* 0=空槽 1=有效数据 */
} CDInfo;

/*
 * 初始化缓存，清空所有槽位
 * 在 main.c 的 prx_start 里调用一次
 */
void mb_cache_init(void);

/*
 * 查缓存，找到返回指针，未命中返回NULL
 */
CDInfo *mb_cache_get(uint32_t discid);

/*
 * 写入缓存，LRU淘汰最久未用的槽位
 */
void mb_cache_put(uint32_t discid, const CDInfo *info);

/*
 * 通过disc ID查询MusicBrainz API
 * 成功把结果写入out并返回0，失败返回-1
 * discid_str: CDDB discid 或其他可用占位符字符串
 * toc_query: 可选的TOC字符串，例如 "1+13+200000+150+15000+..."
 */
int mb_query(const char *discid_str, const char *toc_query, CDInfo *out);

/*
 * 将CDInfo转换为AmgAlbum格式，用于构建AMG二进制响应
 * 转换后的指针指向CDInfo内部的缓冲区，不需要额外内存
 */
void cdinfo_to_amg_album(const CDInfo *info, AmgAlbum *album, AmgTrack *tracks);
