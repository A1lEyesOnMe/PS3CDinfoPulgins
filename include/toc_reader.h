#pragma once
#include <stddef.h>
#include <stdint.h>

#define MAX_TRACKS 99

/* 单条轨道信息 */
typedef struct {
    uint8_t  minute;        /* MSF格式：分 */
    uint8_t  second;        /* MSF格式：秒 */
    uint8_t  frame;         /* MSF格式：帧 */
    uint32_t offset;        /* 换算后的帧偏移量（75帧=1秒） */
} TrackEntry;

/* 完整TOC表 */
typedef struct {
    uint8_t    first_track;             /* 第一条轨道编号，通常为1 */
    uint8_t    last_track;              /* 最后一条轨道编号 */
    uint32_t   leadout;                 /* leadout轨道的帧偏移，代表CD总长度 */
    TrackEntry tracks[MAX_TRACKS + 1];  /* 轨道数组，index 0 不使用 */
} CdromToc;

/*
 * 从光驱读取TOC表
 * 成功返回 0，失败返回 -1
 */
int toc_read(CdromToc *toc);

/*
 * 将 TOC 格式化为 MusicBrainz fuzzy lookup 所需的查询参数：
 * "FIRST+LAST+LEADOUT+OFFSET1+OFFSET2+..."
 *
 * 成功返回实际写入长度，失败返回 -1。
 */
int toc_format_query(const CdromToc *toc, char *out, size_t out_size);

/*
 * 根据TOC计算FreeDB格式的disc ID
 * 返回32位无符号整数
 */
uint32_t toc_calc_discid(const CdromToc *toc);

/*
 * 将disc ID格式化为8位hex字符串
 * out必须至少9字节（8位hex + '\0'）
 * 例如："a50d4a0c"
 */
void toc_discid_str(const CdromToc *toc, char out[9]);
