#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <ctype.h>
#include <http/http.h>
#include <http/https.h>
#include <net/net.h>
#include <ssl/ssl.h>
#include <sysmodule/sysmodule.h>
#include "musicbrainz.h"

/* ------------------------------------------------------------------ */
/* HTTP内存池                                                           */
/* ------------------------------------------------------------------ */

#define HTTP_POOL_SIZE (128 * 1024)  // 128KB
static uint8_t http_pool[HTTP_POOL_SIZE] __attribute__((aligned(128)));

#define SSL_POOL_SIZE (256 * 1024)
static uint8_t ssl_pool[SSL_POOL_SIZE] __attribute__((aligned(128)));

#define CERT_POOL_SIZE (64 * 1024)
static char cert_pool[CERT_POOL_SIZE] __attribute__((aligned(128)));
static httpsData ca_data;

#define URI_POOL_SIZE (4 * 1024)  // 4KB for URI parsing
static uint8_t uri_pool[URI_POOL_SIZE] __attribute__((aligned(128)));

#define RESPONSE_SIZE (64 * 1024)
static char response[RESPONSE_SIZE];

/* ------------------------------------------------------------------ */
/* LRU 缓存                                                             */
/* ------------------------------------------------------------------ */

static CDInfo   cache[CACHE_SIZE];
static uint32_t lru_order[CACHE_SIZE];
static uint32_t lru_clock = 0;

static int load_module(int id) {
    s32 result = sysModuleLoad(id);
    if (result == 0 || (u32)result == SYSMODULE_ERR_DUPLICATE) return 0;
    return -1;
}

/*
 * 分步初始化：每一步成功后单独记状态，任何一步失败都允许下次重试。
 * 插件由 boot_plugins.txt 在开机阶段加载，常常早于网络栈就绪；
 * 旧的整段初始化一次失败就永久锁死，导致之后永远查不到数据。
 */
static int modules_ready = 0;
static int net_ready     = 0;
static int http_ready    = 0;
static int ssl_ready     = 0;
static int https_ready   = 0;

static int mb_http_init(void) {
    u32 cert_size = 0;

    if (https_ready) return 0;

    if (!modules_ready) {
        if (load_module(SYSMODULE_NET) < 0 ||
            load_module(SYSMODULE_HTTP) < 0 ||
            load_module(SYSMODULE_HTTPS) < 0 ||
            load_module(SYSMODULE_SSL) < 0) {
            return -1;
        }
        modules_ready = 1;
    }
    if (!net_ready) {
        if (netInitialize() < 0) return -1;
        net_ready = 1;
    }
    if (!http_ready) {
        if (httpInit(http_pool, HTTP_POOL_SIZE) < 0) return -1;
        http_ready = 1;
    }
    if (!ssl_ready) {
        if (sslInit(ssl_pool, SSL_POOL_SIZE) < 0) return -1;
        ssl_ready = 1;
    }
    if (sslCertificateLoader(SSL_LOAD_CERT_ALL, NULL, 0, &cert_size) < 0 ||
        cert_size == 0 || cert_size > sizeof(cert_pool)) {
        return -1;
    }
    if (sslCertificateLoader(SSL_LOAD_CERT_ALL, cert_pool,
                             sizeof(cert_pool), NULL) < 0) {
        return -1;
    }

    ca_data.ptr = cert_pool;
    ca_data.size = cert_size;
    if (httpsInit(1, &ca_data) < 0) return -1;

    https_ready = 1;
    return 0;
}

void mb_cache_init(void) {
    memset(cache,     0, sizeof(cache));
    memset(lru_order, 0, sizeof(lru_order));
    lru_clock = 0;

    mb_http_init();
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
            cache[i].discid    = discid;
            cache[i].valid     = 1;
            cache[i].not_found = info->not_found;
            lru_order[i]       = ++lru_clock;
            return;
        }
    }
    int slot = lru_evict();
    memcpy(&cache[slot], info, sizeof(CDInfo));
    cache[slot].discid    = discid;
    cache[slot].valid     = 1;
    cache[slot].not_found = info->not_found;
    lru_order[slot]       = ++lru_clock;
}

/* ------------------------------------------------------------------ */
/* JSON解析                                                              */
/* ------------------------------------------------------------------ */

static const char *json_find_value(const char *json, const char *key) {
    char pattern[64];
    int written;
    const char *p;

    if (!json || !key) return NULL;
    written = snprintf(pattern, sizeof(pattern), "\"%s\"", key);
    if (written < 0 || (size_t)written >= sizeof(pattern)) return NULL;

    p = strstr(json, pattern);
    if (!p) return NULL;
    p += strlen(pattern);
    while (*p && isspace((unsigned char)*p)) p++;
    if (*p++ != ':') return NULL;
    while (*p && isspace((unsigned char)*p)) p++;
    return p;
}

static int json_get_str(const char *json, const char *key,
                        char *out, size_t out_size) {
    const char *p = json_find_value(json, key);

    if (!p || *p != '"' || !out || out_size == 0) return -1;
    p++;

    size_t i = 0;
    while (*p && *p != '"' && i < out_size - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    return (i > 0) ? 0 : -1;
}

/*
 * 从 start 处的 open 字符开始，找到与之匹配的 close 字符的位置。
 * 正确跳过字符串字面量与 \" 转义；未找到返回 NULL。
 */
static const char *json_match_bracket(const char *start, char open, char close) {
    int depth = 0;
    int in_string = 0;
    const char *p = start;

    while (*p) {
        if (in_string) {
            if (*p == '\\' && p[1]) p++;      /* 跳过转义字符 */
            else if (*p == '"') in_string = 0;
        } else if (*p == '"') {
            in_string = 1;
        } else if (*p == open) {
            depth++;
        } else if (*p == close) {
            depth--;
            if (depth == 0) return p;
        }
        p++;
    }
    return NULL;
}

static int json_get_tracks(const char *json, CDInfo *out) {
    const char *p = json_find_value(json, "tracks");
    if (!p || *p != '[') return -1;

    const char *arr_end = json_match_bracket(p, '[', ']');
    if (!arr_end) return -1;

    out->track_count = 0;
    p++;

    while (p < arr_end && out->track_count < MAX_TRACKS) {
        const char *track_start = strchr(p, '{');
        if (!track_start || track_start >= arr_end) break;

        const char *obj_end = json_match_bracket(track_start, '{', '}');
        if (!obj_end || obj_end > arr_end) break;

        const char *title_ptr = json_find_value(track_start, "title");
        if (!title_ptr || title_ptr >= obj_end || *title_ptr != '"') {
            /* 缺标题的 track 对象跳过，不中断整张曲目列表 */
            p = obj_end + 1;
            continue;
        }
        title_ptr++;

        size_t i = 0;
        while (*title_ptr && *title_ptr != '"' && i < MAX_TRACK_LEN - 1) {
            out->tracks[out->track_count][i++] = *title_ptr++;
        }
        out->tracks[out->track_count][i] = '\0';
        out->track_count++;

        p = obj_end + 1;
    }
    return (out->track_count > 0) ? 0 : -1;
}

static const char *json_find_release_block(const char *json) {
    const char *p = json_find_value(json, "releases");
    if (!p || *p != '[') return NULL;
    p++;
    while (*p && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t')) p++;
    return (*p == '{') ? p : NULL;
}

static int json_get_release_str(const char *json, const char *key,
                                char *out, size_t out_size) {
    const char *release = json_find_release_block(json);
    if (!release) return -1;
    return json_get_str(release, key, out, out_size);
}

static int json_extract_artist_credit(const char *json, char *out, size_t out_size) {
    const char *p = json_find_value(json, "artist-credit");
    if (!p || *p != '[') return -1;

    const char *arr_end = json_match_bracket(p, '[', ']');
    if (!arr_end) return -1;
    p++;

    size_t written = 0;
    const char *item = p;
    while (item < arr_end && written + 1 < out_size) {
        const char *obj_start = strchr(item, '{');
        if (!obj_start || obj_start >= arr_end) break;

        const char *obj_end = json_match_bracket(obj_start, '{', '}');
        if (!obj_end || obj_end > arr_end) break;
        const char *name_ptr = NULL;
        const char *search = obj_start;
        while (1) {
            const char *next = json_find_value(search, "name");
            if (!next || next >= obj_end || *next != '"') break;
            name_ptr = next + 1;
            search = name_ptr;
        }

        if (!name_ptr) break;

        size_t name_len = 0;
        const char *name_end = name_ptr;
        while (*name_end && *name_end != '"' && name_len < MAX_STR_LEN - 1) {
            name_end++;
            name_len++;
        }

        char name[MAX_STR_LEN];
        if (name_len >= sizeof(name)) name_len = sizeof(name) - 1;
        memcpy(name, name_ptr, name_len);
        name[name_len] = '\0';

        const char *join_ptr = json_find_value(obj_start, "joinphrase");
        char joinphrase[MAX_STR_LEN] = "";
        if (join_ptr && join_ptr < obj_end && *join_ptr == '"') {
            join_ptr++;
            size_t join_len = 0;
            while (*join_ptr && *join_ptr != '"' && join_len < MAX_STR_LEN - 1) {
                joinphrase[join_len++] = *join_ptr++;
            }
            joinphrase[join_len] = '\0';
        }

        size_t to_copy = strlen(name);
        if (written + to_copy + 1 >= out_size) break;
        memcpy(out + written, name, to_copy);
        written += to_copy;
        out[written] = '\0';

        to_copy = strlen(joinphrase);
        if (to_copy > 0) {
            if (written + to_copy + 1 >= out_size) break;
            memcpy(out + written, joinphrase, to_copy);
            written += to_copy;
            out[written] = '\0';
        }

        item = obj_end + 1;
    }

    return (written > 0) ? 0 : -1;
}

/* ------------------------------------------------------------------ */
/* HTTP请求                                                              */
/* ------------------------------------------------------------------ */

int mb_query(const char *discid_str, const char *toc_query, CDInfo *out) {
    if (!discid_str || !discid_str[0] || !out) return -1;
    if (mb_http_init() < 0) return -1;

    uint32_t id = (uint32_t)strtoul(discid_str, NULL, 16);
    CDInfo *cached = mb_cache_get(id);
    if (cached) {
        if (cached->not_found) return -1;   /* 负缓存：MusicBrainz 已确认无此盘 */
        memcpy(out, cached, sizeof(CDInfo));
        return 0;
    }

    char url_str[1400];
    int url_length;
    if (toc_query && toc_query[0]) {
        url_length = snprintf(url_str, sizeof(url_str),
                "https://musicbrainz.org/ws/2/discid/-?fmt=json&inc=artist-credits+recordings&cdstubs=no&toc=%s",
                toc_query);
    } else {
        url_length = snprintf(url_str, sizeof(url_str),
                "https://musicbrainz.org/ws/2/discid/%s?fmt=json&inc=artist-credits+recordings&cdstubs=no",
                discid_str);
    }
    if (url_length < 0 || (size_t)url_length >= sizeof(url_str)) return -1;

    httpUri uri;
    u32 required;
    if (httpUtilParseUri(&uri, url_str, uri_pool, URI_POOL_SIZE, &required) < 0) {
        return -1;
    }

    httpClientId client;
    if (httpCreateClient(&client) < 0) return -1;
    httpClientSetUserAgent(client, "PS3_CD_Info_Plugin/1.0");
    httpClientSetAutoRedirect(client, 1);
    /*
     * mb_query 运行在 hook 拦截的 XMB recv 调用里，必须限时：
     * 无超时时，MusicBrainz 端一次挂起就会让整个 XMB 网络线程卡死。
     */
    httpClientSetConnTimeout(client, 10 * 1000 * 1000);
    httpClientSetRecvTimeout(client, 10 * 1000 * 1000);

    httpTransId trans;
    if (httpCreateTransaction(&trans, client, HTTP_METHOD_GET, &uri) < 0) {
        httpDestroyClient(client);
        return -1;
    }

    httpHeader accept_hdr = {
        .name = "Accept",
        .value = "application/json"
    };
    httpRequestAddHeader(trans, &accept_hdr);

    httpHeader host_hdr = {
        .name = "Host",
        .value = "musicbrainz.org"
    };
    httpRequestAddHeader(trans, &host_hdr);

    u32 sent = 0;
    if (httpSendRequest(trans, NULL, 0, &sent) < 0) {
        httpDestroyTransaction(trans);
        httpDestroyClient(client);
        return -1;
    }

    size_t total = 0;
    u32 recvd;
    int32_t status_code = 0;
    while (httpRecvResponse(trans, response + total,
                           sizeof(response) - total - 1, &recvd) >= 0
           && recvd > 0) {
        total += recvd;
        if (total >= sizeof(response) - 1) break;
    }
    response[total] = '\0';

    if (httpResponseGetStatusCode(trans, &status_code) < 0) {
        httpDestroyTransaction(trans);
        httpDestroyClient(client);
        return -1;
    }
    if (status_code != HTTP_STATUS_CODE_OK) {
        httpDestroyTransaction(trans);
        httpDestroyClient(client);
        /*
         * 404 表示 MusicBrainz 没有该 TOC/discard：结果写负缓存，
         * 同一张盘重插时不再打网络。其他状态码视为临时故障，不缓存。
         */
        if (status_code == HTTP_STATUS_CODE_Not_Found) {
            memset(out, 0, sizeof(CDInfo));
            out->discid    = id;
            out->not_found = 1;
            mb_cache_put(id, out);
        }
        return -1;
    }

    httpDestroyTransaction(trans);
    httpDestroyClient(client);

    char *body = strstr(response, "\r\n\r\n");
    if (!body) body = response;
    else body += 4;

    memset(out, 0, sizeof(CDInfo));

    const char *release_block = json_find_release_block(body);
    if (release_block) {
        if (json_extract_artist_credit(release_block, out->artist, MAX_STR_LEN) < 0) {
            json_get_str(release_block, "sort-name", out->artist, MAX_STR_LEN);
            if (!out->artist[0]) {
               json_get_str(release_block, "name", out->artist, MAX_STR_LEN);
            }
        }

        json_get_release_str(body, "title", out->album, MAX_STR_LEN);
        json_get_tracks(body, out);
    }

    out->discid = id;
    out->valid  = (out->artist[0] && out->album[0] && out->track_count > 0) ? 1 : 0;
    if (!out->valid) return -1;

    mb_cache_put(id, out);
    return 0;
}
