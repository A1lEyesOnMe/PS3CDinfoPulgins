#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <http/http.h>
#include <net/net.h>
#include <sysmodule/sysmodule.h>
#include "musicbrainz.h"

/* ------------------------------------------------------------------ */
/* HTTP内存池                                                           */
/* ------------------------------------------------------------------ */

#define HTTP_POOL_SIZE (128 * 1024)  // 128KB
static uint8_t http_pool[HTTP_POOL_SIZE] __attribute__((aligned(128)));

#define URI_POOL_SIZE (4 * 1024)  // 4KB for URI parsing
static uint8_t uri_pool[URI_POOL_SIZE] __attribute__((aligned(128)));

/* ------------------------------------------------------------------ */
/* LRU 缓存                                                             */
/* ------------------------------------------------------------------ */

static CDInfo   cache[CACHE_SIZE];
static uint32_t lru_order[CACHE_SIZE];
static uint32_t lru_clock = 0;
static int      http_initialized = 0;

void mb_cache_init(void) {
    memset(cache,     0, sizeof(cache));
    memset(lru_order, 0, sizeof(lru_order));
    lru_clock = 0;
    
    /* 初始化HTTP模块 */
    if (!http_initialized) {
        sysModuleLoad(SYSMODULE_HTTP);
        httpInit(http_pool, HTTP_POOL_SIZE);
        http_initialized = 1;
    }
}

CDInfo *mb_cache_get(uint32_t discid) {
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (cache[i].valid && cache[i].discid == discid) {
            lru_order[i] = ++lru_clock;
            return &cache[i];
        }
    }
    return NULL;
}

static int lru_evict(void) {
    int oldest_slot = 0;
    uint32_t oldest_time = lru_order[0];
    for (int i = 1; i < CACHE_SIZE; i++) {
        if (!cache[i].valid) return i;
        if (lru_order[i] < oldest_time) {
            oldest_time = lru_order[i];
            oldest_slot = i;
        }
    }
    return oldest_slot;
}

void mb_cache_put(uint32_t discid, const CDInfo *info) {
    for (int i = 0; i < CACHE_SIZE; i++) {
        if (cache[i].valid && cache[i].discid == discid) {
            memcpy(&cache[i], info, sizeof(CDInfo));
            cache[i].discid  = discid;
            cache[i].valid   = 1;
            lru_order[i]     = ++lru_clock;
            return;
        }
    }
    int slot = lru_evict();
    memcpy(&cache[slot], info, sizeof(CDInfo));
    cache[slot].discid = discid;
    cache[slot].valid  = 1;
    lru_order[slot]    = ++lru_clock;
}

/* ------------------------------------------------------------------ */
/* JSON解析                                                              */
/* ------------------------------------------------------------------ */

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

static int json_get_tracks(const char *json, CDInfo *out) {
    const char *p = strstr(json, "\"track-list\":[");
    if (!p) return -1;

    p += strlen("\"track-list\":[");
    out->track_count = 0;

    while (*p && *p != ']' && out->track_count < MAX_TRACKS) {
        const char *title_start = strstr(p, "\"title\":\"");
        if (!title_start || title_start > strstr(p, "]")) break;

        title_start += strlen("\"title\":\"");

        size_t i = 0;
        while (*title_start && *title_start != '"'
               && i < MAX_TRACK_LEN - 1) {
            out->tracks[out->track_count][i++] = *title_start++;
        }
        out->tracks[out->track_count][i] = '\0';
        out->track_count++;

        p = title_start;
    }
    return (out->track_count > 0) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* HTTP请求                                                              */
/* ------------------------------------------------------------------ */

int mb_query(const char *discid_str, CDInfo *out) {
    uint32_t id = (uint32_t)strtoul(discid_str, NULL, 16);
    
    /* 先查缓存 */
    CDInfo *cached = mb_cache_get(id);
    if (cached) {
        memcpy(out, cached, sizeof(CDInfo));
        return 0;
    }

    /* 构造URL字符串 */
    char url_str[256];
    snprintf(url_str, sizeof(url_str),
             "https://musicbrainz.org/ws/2/discid/%s?fmt=json&inc=recordings",
             discid_str);

    /* 解析URL为httpUri结构体 */
    httpUri uri;
    u32 required;
    if (httpUtilParseUri(&uri, url_str, uri_pool, URI_POOL_SIZE, &required) < 0) {
        return -1;
    }

    /* 创建HTTP客户端 */
    httpClientId client;
    if (httpCreateClient(&client) < 0) return -1;

    /* 设置User-Agent */
    httpClientSetUserAgent(client, "PS3_CD_Info_Plugin/1.0");

    /* 创建事务 */
    httpTransId trans;
    if (httpCreateTransaction(&trans, client, HTTP_METHOD_GET, &uri) < 0) {
        httpDestroyClient(client);
        return -1;
    }

    /* 添加Accept头 */
    httpHeader accept_hdr = {
        .name = "Accept",
        .value = "application/json"
    };
    httpRequestAddHeader(trans, &accept_hdr);

    /* 发送请求（GET无body，所以buf=NULL, size=0） */
    u32 sent = 0;
    if (httpSendRequest(trans, NULL, 0, &sent) < 0) {
        httpDestroyTransaction(trans);
        httpDestroyClient(client);
        return -1;
    }

    /* 接收响应 */
    char response[8192];
    int total = 0;
    u32 recvd;
    
    while (httpRecvResponse(trans, response + total,
                            sizeof(response) - total - 1, &recvd) >= 0
           && recvd > 0) {
        total += recvd;
        if (total >= sizeof(response) - 1) break;
    }
    response[total] = '\0';

    httpDestroyTransaction(trans);
    httpDestroyClient(client);

    /* 找JSON body（跳过HTTP headers） */
    char *body = strstr(response, "\r\n\r\n");
    if (!body) return -1;
    body += 4;

    /* 解析JSON */
    memset(out, 0, sizeof(CDInfo));

    if (json_get_str(body, "sort-name", out->artist, MAX_STR_LEN) < 0)
        json_get_str(body, "name", out->artist, MAX_STR_LEN);

    json_get_str(body, "title", out->album, MAX_STR_LEN);
    json_get_tracks(body, out);

    out->discid = id;
    out->valid  = 1;

    /* 写入缓存 */
    mb_cache_put(id, out);

    return (out->artist[0] && out->album[0]) ? 0 : -1;
}