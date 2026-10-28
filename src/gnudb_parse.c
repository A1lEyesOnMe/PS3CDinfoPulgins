#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gnudb.h"

/*
 * 解析 gnudb cddb read 返回的 CDDB 文本记录。
 *
 * 记录形如：
 *   210 rock 8a0d4a0c
 *   # Track frame offsets:
 *   # 150 ...
 *   DISCID=8a0d4a0c
 *   DTITLE=Artist / Album
 *   DYEAR=1999
 *   DGENRE=Rock
 *   TTITLE0=Track One
 *   TTITLE1=Track Two
 *   EXTD=...
 *   EXTT0=...
 *   .
 *
 * 规则（CDDB 文本协议）：
 *   - '#' 开头为注释；
 *   - "NAME=value" 为字段赋值；
 *   - 不含 '=' 的行为上一字段的续行，按 CDDB 惯例以换行衔接，
 *     这里合并为一个空格，避免 XMB 显示控制字符；
 *   - 单独一行的 "." 表示记录结束。
 *
 * 纯字符串逻辑，不依赖任何 PS3 头文件，主机侧可直接单测。
 */

/* 判断行首字段名并返回 '=' 之后的位置；不匹配返回 NULL */
static const char *match_field(const char *line, const char *name) {
    size_t name_len = strlen(name);
    if (strncmp(line, name, name_len) != 0) return NULL;
    if (line[name_len] != '=') return NULL;
    return line + name_len + 1;
}

/* 把 value 拷入 out（容量 cap），返回 out 便于续行跟踪 */
static char *copy_value(char *out, size_t cap, const char *value) {
    size_t i = 0;
    while (value[i] && i < cap - 1) {
        out[i] = value[i];
        i++;
    }
    out[i] = '\0';
    return out;
}

int gnudb_parse_record(const char *record, CDInfo *out) {
    if (!record || !out) return -1;
    memset(out, 0, sizeof(*out));

    /* 续行目标：最近一次赋值的字段缓冲区及其容量 */
    char  *field_buf  = NULL;
    size_t field_cap  = 0;

    const char *p = record;
    while (*p) {
        const char *eol = strchr(p, '\n');
        size_t line_len = eol ? (size_t)(eol - p) : strlen(p);

        char line[256];
        if (line_len >= sizeof(line)) line_len = sizeof(line) - 1;
        memcpy(line, p, line_len);
        line[line_len] = '\0';
        p = eol ? eol + 1 : p + line_len;

        /* 去掉行尾 \r 与尾部空白 */
        size_t l = strlen(line);
        while (l > 0 && (line[l - 1] == '\r' || line[l - 1] == ' '
                         || line[l - 1] == '\t')) {
            line[--l] = '\0';
        }

        if (line[0] == '.' && line[1] == '\0') break;  /* 记录结束 */
        if (line[0] == '#') continue;                  /* 注释 */

        if (!strchr(line, '=')) {
            /* 续行：追加到上一字段（一个空格衔接） */
            if (field_buf && field_buf[0] && l > 0 &&
                strlen(field_buf) + 1 + l < field_cap) {
                size_t used = strlen(field_buf);
                field_buf[used] = ' ';
                memcpy(field_buf + used + 1, line, l + 1);
            }
            continue;
        }

        const char *value;
        if ((value = match_field(line, "DTITLE")) != NULL) {
            /* DTITLE=Artist / Album：按第一个 " / " 分割 */
            const char *sep = strstr(value, " / ");
            if (sep) {
                copy_value(out->artist, sizeof(out->artist), value);
                out->artist[sep - value] = '\0';
                copy_value(out->album, sizeof(out->album), sep + 3);
                field_buf = out->album;
                field_cap = sizeof(out->album);
            } else {
                /* 无分隔符：整段作为专辑名，艺术家留空 */
                copy_value(out->album, sizeof(out->album), value);
                field_buf = out->album;
                field_cap = sizeof(out->album);
            }
            continue;
        }

        /* TTITLE<编号>=：编号是字段名的一部分，不能用精确字段名匹配 */
        if (strncmp(line, "TTITLE", 6) == 0 &&
            line[6] >= '0' && line[6] <= '9') {
            char *end;
            long n = strtol(line + 6, &end, 10);
            if (end == line + 6 || *end != '=' || n < 0 || n >= MAX_TRACKS) {
                continue;
            }
            value = end + 1;
            copy_value(out->tracks[n], MAX_TRACK_LEN, value);
            if ((uint32_t)(n + 1) > out->track_count) {
                out->track_count = (uint8_t)(n + 1);
            }
            field_buf = out->tracks[n];
            field_cap = MAX_TRACK_LEN;
            continue;
        }

        /* DISCID/DYEAR/DGENRE/EXTD/EXTTn/PLYLONG... 等字段：忽略，
           但 EXTn 不是续行目标，重置续行状态防止串接 */
        field_buf = NULL;
        field_cap = 0;
    }

    out->valid = (out->album[0] && out->track_count > 0) ? 1 : 0;
    return out->valid ? 0 : -1;
}
