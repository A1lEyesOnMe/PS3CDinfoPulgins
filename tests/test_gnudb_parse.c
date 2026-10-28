#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "../include/gnudb.h"

int main() {
    printf("Testing gnudb record parser...\n");

    /* 完整记录：含注释、多字段、续行、EXT 字段与结束标记 */
    const char *record =
        "210 rock 8a0d4a0c\r\n"
        "# Track frame offsets:\r\n"
        "# 150\r\n"
        "# 15000\r\n"
        "DISCID=8a0d4a0c\r\n"
        "DTITLE=Queen / A Night at the Opera\r\n"
        "DYEAR=1975\r\n"
        "DGENRE=Rock\r\n"
        "TTITLE0=Death on Two Legs\r\n"
        "TTITLE1=Lazing on a\r\n"
        "Sunday Afternoon\r\n"
        "TTITLE2=Seaside Rendezvous\r\n"
        "EXTD=submitted via gnudb\r\n"
        "EXTT0=XXXX\r\n"
        ".\r\n";

    CDInfo info;
    assert(gnudb_parse_record(record, &info) == 0);
    assert(info.valid == 1);
    assert(strcmp(info.artist, "Queen") == 0);
    assert(strcmp(info.album, "A Night at the Opera") == 0);
    assert(info.track_count == 3);
    assert(strcmp(info.tracks[0], "Death on Two Legs") == 0);
    assert(strcmp(info.tracks[1], "Lazing on a Sunday Afternoon") == 0);
    assert(strcmp(info.tracks[2], "Seaside Rendezvous") == 0);
    printf("✅ Parsed artist/album/tracks with continuation line\n");

    /* DTITLE 没有 " / " 分隔符：整段作为专辑名，艺术家为空 */
    const char *no_artist =
        "210 misc 12345678\r\n"
        "DTITLE=Symphony No. 5\r\n"
        "TTITLE0=First Movement\r\n"
        ".\r\n";

    CDInfo info2;
    assert(gnudb_parse_record(no_artist, &info2) == 0);
    assert(info2.artist[0] == '\0');
    assert(strcmp(info2.album, "Symphony No. 5") == 0);
    assert(info2.track_count == 1);
    printf("✅ DTITLE without artist handled\n");

    /* 没有 TTITLE 字段：记录无效 */
    const char *no_tracks =
        "210 rock 8a0d4a0c\r\n"
        "DTITLE=Artist / Album\r\n"
        ".\r\n";

    CDInfo info3;
    assert(gnudb_parse_record(no_tracks, &info3) < 0);
    printf("✅ Record without tracks rejected\n");

    /* 缺少结束标记的截断记录也应安全解析（容错） */
    const char *truncated =
        "210 rock 8a0d4a0c\r\n"
        "DTITLE=Artist / Album\r\n"
        "TTITLE0=Track\r\n";

    CDInfo info4;
    assert(gnudb_parse_record(truncated, &info4) == 0);
    assert(info4.track_count == 1);
    printf("✅ Truncated record handled safely\n");

    printf("✅ All gnudb parser tests passed!\n");
    return 0;
}
