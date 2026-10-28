#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "../include/gnudb.h"
#include "../include/formatter.h"

int main() {
    printf("Testing CDDB formatter...\n");
    
    /* 构造测试数据 */
    CDInfo info = {
        .artist = "Test Artist",
        .album = "Test Album",
        .track_count = 3,
        .discid = 0xa50d4a0c,
        .valid = 1
    };
    
    strcpy(info.tracks[0], "Track 1");
    strcpy(info.tracks[1], "Track 2");
    strcpy(info.tracks[2], "Track 3");
    
    /* 格式化 */
    char buf[4096];
    int len = format_cddb_response(buf, sizeof(buf), &info);
    
    assert(len > 0);
    printf("Generated response (%d bytes):\n%s\n", len, buf);
    
    /* 验证格式 */
    assert(strstr(buf, "210 OK") != NULL);
    assert(strstr(buf, "DTITLE=Test Artist / Test Album") != NULL);
    assert(strstr(buf, "TTITLE0=Track 1") != NULL);
    assert(strstr(buf, "TTITLE1=Track 2") != NULL);
    assert(strstr(buf, "TTITLE2=Track 3") != NULL);
    assert(strstr(buf, ".\r\n") != NULL);
    
    printf("✅ All tests passed!\n");
    return 0;
}