#include <stdio.h>
#include <string.h>
#include <assert.h>

/* 从 musicbrainz.c 复制json_get_str函数 */
static int json_get_str(const char *json, const char *key,
                         char *out, size_t out_size) {
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    const char *p = strstr(json, pattern);
    if (!p) {
        snprintf(pattern, sizeof(pattern), "\"%s\": \"", key);
        p = strstr(json, pattern);
    }
    if (!p) return -1;
    p += strlen(pattern);
    size_t i = 0;
    while (*p && *p != '"' && i < out_size - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (i > 0) ? 0 : -1;
}

int main() {
    printf("Testing JSON parser...\n");
    
    const char *test_json = "{\"title\":\"Test Album\",\"sort-name\":\"Test Artist\"}";
    
    char title[128];
    char artist[128];
    
    assert(json_get_str(test_json, "title", title, sizeof(title)) == 0);
    assert(strcmp(title, "Test Album") == 0);
    
    assert(json_get_str(test_json, "sort-name", artist, sizeof(artist)) == 0);
    assert(strcmp(artist, "Test Artist") == 0);
    
    printf("✅ JSON parser works!\n");
    return 0;
}