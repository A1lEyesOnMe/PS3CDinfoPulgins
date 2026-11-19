#include <stdio.h>
#include <string.h>
#include "formatter.h"
#include "amg_response.h"
#include "musicbrainz.h"

/* Use AMG constants for buffer size validation */
#define MIN_AMG_BUF_SIZE AMG_MIN_BUF_SIZE
#define MIN_AMG_SCRATCH_SIZE AMG_MIN_SCRATCH_SIZE

/*
 * 格式化CDDB协议响应（保留向后兼容）
 * 
 * PS3 XMB期待的CDDB格式：
 * 1. 状态行：210 OK (表示查询成功)
 * 2. DTITLE=艺术家 / 专辑名
 * 3. 每首歌：TTITLE0=..., TTITLE1=..., ...
 * 4. 结束标记：单独一行的点号 "."
 */
int format_cddb_response(char *buf, size_t buf_size, const CDInfo *info) {
    /* 检查输入合法性 */
    if (!buf || !info || buf_size < 128) return -1;
    if (!info->valid || !info->artist[0] || !info->album[0]) return -1;

    char *p = buf;
    char *end = buf + buf_size;
    int written;

    /* 状态行：210 OK */
    written = snprintf(p, end - p, "210 OK\r\n");
    if (written < 0 || p + written >= end) return -1;
    p += written;

    /* DTITLE=Artist / Album */
    written = snprintf(p, end - p, "DTITLE=%s / %s\r\n",
                       info->artist, info->album);
    if (written < 0 || p + written >= end) return -1;
    p += written;

    /* 逐条输出歌曲列表 */
    for (int i = 0; i < info->track_count && i < MAX_TRACKS; i++) {
        /* 空曲目名跳过，防止显示异常 */
        if (!info->tracks[i][0]) continue;

        /* TTITLE{N}=歌曲名 */
        written = snprintf(p, end - p, "TTITLE%d=%s\r\n",
                           i, info->tracks[i]);
        if (written < 0 || p + written >= end) return -1;
        p += written;
    }

    /* CDDB协议结束标记：单独一行的点号 */
    written = snprintf(p, end - p, ".\r\n");
    if (written < 0 || p + written >= end) return -1;
    p += written;

    /* 返回实际写入的字节数 */
    return (int)(p - buf);
}

/*
 * 构建AMG二进制响应（XMB原生格式）
 * 
 * 这是PS3 XMB真正期望的格式，而非CDDB文本格式。
 * 使用从simple-cd-info借鉴的AMG二进制协议构建。
 * 
 * buf: 输出缓冲区
 * buf_size: 缓冲区大小
 * info: CD信息
 * scratch: 临时工作区（用于构建嵌套容器）
 * scratch_size: 临时工作区大小
 * 
 * 返回：实际写入的字节数，失败返回-1
 */
int format_amg_response(char *buf, size_t buf_size, const CDInfo *info,
                        unsigned char *scratch, size_t scratch_size) {
    if (!buf || !info || !scratch || buf_size < MIN_AMG_BUF_SIZE || scratch_size < MIN_AMG_SCRATCH_SIZE) return -1;
    if (!info->valid || !info->artist[0] || !info->album[0]) return -1;

    AmgAlbum album;
    AmgTrack tracks[MAX_TRACKS];

    cdinfo_to_amg_album(info, &album, tracks);

    return buildAmgResponse(&album, scratch, (int)scratch_size,
                            (unsigned char *)buf, (int)buf_size);
}
