#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hook_net.h"
#include "toc_reader.h"

/*
 * 从CDDB请求中提取disc ID 与 TOC 参数。
 * 格式: "cddb query a50d4a0c 13 150 15000 ..."
 *
 * 独立成文件的原因：本函数是纯字符串逻辑，不依赖任何 PS3 头文件，
 * 抽出后主机侧单元测试（tests/test_cddb_query.c）可以直接编译验证，
 * 无需 PS3 工具链。
 */
int parse_cddb_query(const char *request, char *discid_out,
                     size_t discid_out_size,
                     char *toc_out, size_t toc_out_size) {
    if (!request || !discid_out || discid_out_size < 9 ||
        !toc_out || toc_out_size == 0) {
        return -1;
    }

    const char *p = strstr(request, "cddb query ");
    if (!p) p = strstr(request, "CDDB query ");
    if (!p) return -1;

    p += strlen("cddb query ");
    while (*p == ' ') p++;

    size_t i = 0;
    while (*p && *p != ' ' && *p != '\r' && *p != '\n' && i < discid_out_size - 1) {
        discid_out[i++] = *p++;
    }
    discid_out[i] = '\0';
    if (i != 8) return -1;
    for (size_t j = 0; j < 8; j++) {
        char c = discid_out[j];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return -1;
        }
    }

    while (*p == ' ') p++;
    char *endptr;
    long track_count = strtol(p, &endptr, 10);
    if (track_count <= 0 || track_count > MAX_TRACKS) return -1;
    p = endptr;

    uint32_t offsets[MAX_TRACKS] = {0};
    for (int idx = 0; idx < track_count; idx++) {
        while (*p == ' ') p++;
        if (!*p || *p == '\r' || *p == '\n') return -1;
        char *next;
        long offset = strtol(p, &next, 10);
        if (next == p || offset < 0 || (uint64_t)offset > UINT32_MAX) {
            return -1;
        }
        if (idx > 0 && (uint32_t)offset <= offsets[idx - 1]) return -1;
        offsets[idx] = (uint32_t)offset;
        p = next;
    }

    /*
     * CDDB 的最后一个字段是从第一轨到 lead-out 的秒数，不是帧偏移。
     * fuzzy TOC 需要绝对帧，因此按 FreeDB 的 disc length 定义还原。
     */
    while (*p == ' ') p++;
    char *next;
    long total_seconds = strtol(p, &next, 10);
    if (next == p || total_seconds <= 0 ||
        (uint64_t)total_seconds > UINT32_MAX / 75U) {
        return -1;
    }
    uint64_t leadout = ((uint64_t)total_seconds +
                        offsets[0] / 75U) * 75U;
    if (leadout > UINT32_MAX ||
        leadout <= offsets[track_count - 1]) {
        return -1;
    }

    int written = snprintf(toc_out, toc_out_size, "1+%ld+%u",
                           track_count, (uint32_t)leadout);
    if (written < 0 || (size_t)written >= toc_out_size) return -1;

    for (int idx = 0; idx < track_count; idx++) {
        size_t remaining = toc_out_size - (size_t)written;
        int appended = snprintf(toc_out + written, remaining, "+%u",
                                offsets[idx]);
        if (appended < 0 || (size_t)appended >= remaining) return -1;
        written += appended;
    }

    return 0;
}
