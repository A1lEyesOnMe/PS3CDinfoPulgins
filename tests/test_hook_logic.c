#include <stdio.h>
#include <string.h>
#include <assert.h>

/* 测试disc ID提取 */
static int parse_cddb_discid(const char *request, char *out, size_t out_size) {
    const char *p = strstr(request, "cddb query ");
    if (!p) return -1;
    p += strlen("cddb query ");
    while (*p == ' ') p++;
    size_t i = 0;
    while (*p && *p != ' ' && *p != '\r' && *p != '\n' && i < out_size - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    if (i != 8) return -1;
    return 0;
}

int main() {
    printf("Testing CDDB request parsing...\n");
    
    const char *test_request = "cddb query a50d4a0c 13 150 15000 28000 42000\r\n";
    char discid[16];
    
    assert(parse_cddb_discid(test_request, discid, sizeof(discid)) == 0);
    assert(strcmp(discid, "a50d4a0c") == 0);
    
    printf("✅ Extracted disc ID: %s\n", discid);
    return 0;
}