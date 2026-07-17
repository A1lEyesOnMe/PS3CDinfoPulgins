#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "toc_reader.h"

/*
 * TODO: 实际的TOC读取需要SCSI命令
 * PSL1GHT可能需要用libcdvd或直接syscall
 * 
 * 暂时用硬编码测试数据
 */
int toc_read(CdromToc *toc) {
    /* 硬编码一张测试CD的TOC（13轨） */
    toc->first_track = 1;
    toc->last_track  = 13;
    
    /* 模拟轨道偏移（单位：帧，75帧=1秒） */
    toc->tracks[1].offset  = 150;      /* 第1轨从2秒开始 */
    toc->tracks[2].offset  = 15000;
    toc->tracks[3].offset  = 28000;
    toc->tracks[4].offset  = 42000;
    toc->tracks[5].offset  = 58000;
    toc->tracks[6].offset  = 72000;
    toc->tracks[7].offset  = 88000;
    toc->tracks[8].offset  = 103000;
    toc->tracks[9].offset  = 118000;
    toc->tracks[10].offset = 135000;
    toc->tracks[11].offset = 152000;
    toc->tracks[12].offset = 168000;
    toc->tracks[13].offset = 185000;
    
    toc->leadout = 200000;  /* CD总长度 */
    
    return 0;
}

/* FreeDB校验和辅助函数 */
static uint32_t cddb_sum(uint32_t n) {
    uint32_t ret = 0;
    while (n > 0) {
        ret += n % 10;
        n   /= 10;
    }
    return ret;
}

/* 计算FreeDB disc ID */
uint32_t toc_calc_discid(const CdromToc *toc) {
    uint32_t checksum = 0;
    
    for (int i = toc->first_track; i <= toc->last_track; i++) {
        checksum += cddb_sum(toc->tracks[i].offset / 75);
    }
    
    uint32_t total_secs =
        toc->leadout / 75 - toc->tracks[toc->first_track].offset / 75;
    
    uint32_t track_count =
        (uint32_t)(toc->last_track - toc->first_track + 1);
    
    return ((checksum % 0xFF) << 24)
         | (total_secs        << 8)
         |  track_count;
}

/* 格式化为8位hex字符串 */
void toc_discid_str(const CdromToc *toc, char out[9]) {
    uint32_t id = toc_calc_discid(toc);
    snprintf(out, 9, "%08x", id);
}