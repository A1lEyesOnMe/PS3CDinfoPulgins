#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <stdint.h>
#include <http/http.h>
#include <net/net.h>
#include <sysmodule/sysmodule.h>
#include "gnudb.h"

/* ------------------------------------------------------------------ */
/* HTTP内存池（纯 HTTP，无 SSL）                                          */
/* ------------------------------------------------------------------ */

#define HTTP_POOL_SIZE (128 * 1024)
static uint8_t http_pool[HTTP_POOL_SIZE] __attribute__((aligned(128)));

#define URI_POOL_SIZE (4 * 1024)
static uint8_t uri_pool[URI_POOL_SIZE] __attribute__((aligned(128)));

#define RESPONSE_SIZE (64 * 1024)
static char response[RESPONSE_SIZE];

/* ------------------------------------------------------------------ */
/* LRU 缓存                                                             */
/* ------------------------------------------------------------------ */

static CDInfo   cache[CACHE_SIZE];
static uint32_t lru_order[CACHE_SIZE];
static uint32_t lru_clock = 0;

/*
 * 分步初始化：每一步成功后单独记状态，任何一步失败都允许下次重试。
 * 插件由 boot_plugins.txt 在开机阶段加载，常常早于网络栈就绪；
 * 旧的整段初始化一次失败就永久锁死，导致之后永远查不到数据。
 */
static int modules_ready = 0;
static int net_ready     = 0;
static int http_ready    = 0;

static int load_module(int id) {
    s32 result = sysModuleLoad(id);
    if (result == 0 || (u32)result == SYSMODULE_ERR_DUPLICATE) return 0;
    return -1;
}

static int gnudb_http_init(void) {
    if (http_ready) return 0;

    if (!modules_ready) {
        if (load_module(SYSMODULE_NET) < 0 ||
            load_module(SYSMODULE_HTTP) < 0) {
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
    return 0;
}

void gnudb_cache_init(void) {
    memset(cache,     0, sizeof(cache));
    memset(lru_order, 0, sizeof(lru_order));
    lru_clock = 0;
}

CDInfo *gnudb_cache_get(uint32_t discid) {
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

void gnudb_cache_put(uint32_t discid, const CDInfo *info) {
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
/* HTTP请求                                                              */
/* ------------------------------------------------------------------ */

/*
 * 一次纯 HTTP GET。body（最多 cap-1 字节）写入 out 并补 NUL；
 * HTTP 状态码写入 *status。收到响应返回 0，传输失败返回 -1。
 * gnudb 是纯 HTTP，无需 SSL 回调。
 */
static int http_get(const char *url, char *out, int cap, int32_t *status) {
    httpUri uri;
    u32 required;
    if (httpUtilParseUri(&uri, url, uri_pool, URI_POOL_SIZE, &required) < 0) {
        return -1;
    }

    httpClientId client;
    if (httpCreateClient(&client) < 0) return -1;
    httpClientSetUserAgent(client, "PS3_CD_Info_Plugin/1.0");
    httpClientSetAutoRedirect(client, 1);
    /*
     * gnudb_query 运行在 hook 拦截的 XMB recv 调用里，必须限时：
     * 无超时时，远端一次挂起就会让整个 XMB 网络线程卡死。
     */
    httpClientSetConnTimeout(client, 10 * 1000 * 1000);
    httpClientSetRecvTimeout(client, 10 * 1000 * 1000);

    httpTransId trans;
    if (httpCreateTransaction(&trans, client, HTTP_METHOD_GET, &uri) < 0) {
        httpDestroyClient(client);
        return -1;
    }

    u32 sent = 0;
    if (httpSendRequest(trans, NULL, 0, &sent) < 0) {
        httpDestroyTransaction(trans);
        httpDestroyClient(client);
        return -1;
    }

    size_t total = 0;
    u32 recvd;
    while (httpRecvResponse(trans, response + total,
                            sizeof(response) - total - 1, &recvd) >= 0
           && recvd > 0) {
        total += recvd;
        if (total >= sizeof(response) - 1) break;
    }
    response[total] = '\0';

    int32_t code = 0;
    if (httpResponseGetStatusCode(trans, &code) < 0) {
        httpDestroyTransaction(trans);
        httpDestroyClient(client);
        return -1;
    }
    *status = code;

    httpDestroyTransaction(trans);
    httpDestroyClient(client);

    /* 剥离响应头，正文挪到调用方缓冲区 */
    char *body = strstr(response, "\r\n\r\n");
    if (!body) body = response;
    else body += 4;

    size_t body_len = strlen(body);
    if ((int)body_len > cap - 1) body_len = (size_t)(cap - 1);
    memcpy(out, body, body_len);
    out[body_len] = '\0';
    return 0;
}

/* 把无符号整数追加到缓冲区（snprintf 拼长 URL 太慢，这里手写） */
static int append_uint(char *out, int cap, int at, uint32_t v) {
    char tmp[12];
    int n = 0;
    if (v == 0) tmp[n++] = '0';
    while (v > 0 && n < (int)sizeof(tmp)) {
        tmp[n++] = (char)('0' + v % 10);
        v /= 10;
    }
    while (n > 0 && at < cap - 1) out[at++] = tmp[--n];
    return at;
}

static int append_str(char *out, int cap, int at, const char *s) {
    while (*s && at < cap - 1) out[at++] = *s++;
    return at;
}

/*
 * 从 toc_query（"FIRST+LAST+LEADOUT+OFF1+..."）构造 gnudb cddb query URL。
 * CDDB query 命令格式：cmd=cddb+query+DISCID+NTRACKS+OFF1+...+OFFn+NSEC
 * 其中 NSEC 是整张盘的秒数 = (LEADOUT - OFF1) / 75。
 */
static int build_query_url(const char *discid_str, const char *toc_query,
                           char *url, int cap) {
    uint32_t fields[MAX_TRACKS + 1];
    int count = 0;
    const char *p = toc_query;

    while (*p && count < (int)(MAX_TRACKS + 1)) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p || v < 0) return -1;
        fields[count++] = (uint32_t)v;
        p = end;
        if (*p == '+') p++;
        else if (*p == '\0') break;
        else return -1;
    }
    /* FIRST LAST LEADOUT + 至少 1 个偏移 */
    if (count < 4) return -1;

    uint32_t first    = fields[0];
    uint32_t last     = fields[1];
    uint32_t leadout  = fields[2];
    int ntracks       = (int)(last - first + 1);
    int offset_count  = count - 3;
    if (ntracks < 1 || ntracks > MAX_TRACKS || offset_count != ntracks) {
        return -1;
    }
    if (leadout <= fields[3]) return -1;
    uint32_t nsec = (leadout - fields[3]) / 75;

    int at = 0;
    at = append_str(url, cap, at, "http://gnudb.gnudb.org/~cddb/cddb.cgi?cmd=cddb+query+");
    at = append_str(url, cap, at, discid_str);
    for (int i = 0; i < ntracks; i++) {
        at = append_str(url, cap, at, "+");
        at = append_uint(url, cap, at, fields[3 + i]);
    }
    at = append_str(url, cap, at, "+");
    at = append_uint(url, cap, at, nsec);
    at = append_str(url, cap, at, "&hello=user+ps3+abcde+2.9.3&proto=6");
    if (at >= cap - 1) return -1;
    url[at] = '\0';
    return 0;
}

/* ------------------------------------------------------------------ */
/* 查询入口                                                              */
/* ------------------------------------------------------------------ */

int gnudb_query(const char *discid_str, const char *toc_query, CDInfo *out) {
    if (!discid_str || !discid_str[0] || !toc_query || !toc_query[0] || !out) {
        return -1;
    }
    if (gnudb_http_init() < 0) return -1;

    uint32_t id = (uint32_t)strtoul(discid_str, NULL, 16);
    CDInfo *cached = gnudb_cache_get(id);
    if (cached) {
        if (cached->not_found) return -1;   /* 负缓存：gnudb 已确认无此盘 */
        memcpy(out, cached, sizeof(CDInfo));
        return 0;
    }

    /* 第一步：cddb query，拿到匹配记录的 category */
    char url[1400];
    char first_line[160];
    if (build_query_url(discid_str, toc_query, url, sizeof(url)) < 0) {
        return -1;
    }

    int32_t status = 0;
    if (http_get(url, first_line, sizeof(first_line), &status) != 0) {
        return -1;
    }

    if (status == 202) {
        /* gnudb 无此盘：结果写负缓存，同一张盘重插时不再打网络 */
        memset(out, 0, sizeof(CDInfo));
        out->discid    = id;
        out->not_found = 1;
        gnudb_cache_put(id, out);
        return -1;
    }
    /* 200 精确匹配 / 211 多重匹配都取第一条记录 */
    if (status != 200 && status != 211) return -1;

    /* 响应行："200 rock 8a0d4a0c Artist / Album" → 取 category */
    char category[24] = {0};
    {
        const char *p = first_line;
        while (*p == ' ' || *p == '\r' || *p == '\n') p++;
        while (*p && *p != ' ') p++;              /* 跳过状态码 */
        while (*p == ' ') p++;
        size_t i = 0;
        while (*p && *p != ' ' && *p != '\r' && *p != '\n' &&
               i < sizeof(category) - 1) {
            category[i++] = *p++;
        }
        category[i] = '\0';
        if (i == 0) return -1;
    }

    /* 第二步：cddb read，取完整记录 */
    int at = 0;
    at = append_str(url, sizeof(url), at,
                    "http://gnudb.gnudb.org/~cddb/cddb.cgi?cmd=cddb+read+");
    at = append_str(url, sizeof(url), at, category);
    at = append_str(url, sizeof(url), at, "+");
    at = append_str(url, sizeof(url), at, discid_str);
    at = append_str(url, sizeof(url), at, "&hello=user+ps3+abcde+2.9.3&proto=6");
    if (at >= (int)sizeof(url) - 1) return -1;
    url[at] = '\0';

    if (http_get(url, response, sizeof(response), &status) != 0 ||
        status != 200) {
        return -1;
    }

    memset(out, 0, sizeof(CDInfo));
    if (gnudb_parse_record(response, out) < 0 || !out->valid) {
        return -1;
    }

    out->discid = id;
    gnudb_cache_put(id, out);
    return 0;
}
