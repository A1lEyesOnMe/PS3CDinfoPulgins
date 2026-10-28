#include <stdio.h>
#include <string.h>
#include "formatter.h"

/*
 * 格式化CDDB协议响应
 * 
 * PS3 XMB期待的CDDB格式：
 * 1. 状态行：210 OK (表示查询成功)
 * 2. DTITLE=艺术家 / 专辑名
 * 3. 每首歌：TTITLE0=..., TTITLE1=..., ...
 * 4. 结束标记：单独一行的点号 "."
 * 
 * 示例输出：
 * 210 OK
 * DTITLE=椎名林檎 / 勝訴ストリップ
 * TTITLE0=本能
 * TTITLE1=罪と罰
 * TTITLE2=丸の内サディスティック
 * .
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

    /* DTITLE=Artist / Album（gnudb 个别记录可能没有艺术家，省略分隔符） */
    if (info->artist[0]) {
        written = snprintf(p, end - p, "DTITLE=%s / %s\r\n",
                           info->artist, info->album);
    } else {
        written = snprintf(p, end - p, "DTITLE=%s\r\n", info->album);
    }
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
