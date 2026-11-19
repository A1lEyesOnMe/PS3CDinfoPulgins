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
static int      http_initialized = 0;

static int load_module(int id) {
    s32 result = sysModuleLoad(id);
    if (result == 0 || (u32)result == SYSMODULE_ERR_DUPLICATE) return 0;
    return -1;
}

static int mb_http_init(void) {
    u32 cert_size = 0;

    if (http_initialized > 0) return 0;
    if (http_initialized < 0) return -1;
    http_initialized = -1;

    if (load_module(SYSMODULE_NET) < 0 ||
        load_module(SYSMODULE_HTTP) < 0 ||
        load_module(SYSMODULE_HTTPS) < 0 ||
        load_module(SYSMODULE_SSL) < 0) {
        return -1;
    }
    if (netInitialize() < 0) return -1;
    if (httpInit(http_pool, HTTP_POOL_SIZE) < 0) return -1;
    if (sslInit(ssl_pool, SSL_POOL_SIZE) < 0) return -1;

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

    http_initialized = 1;
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

static int json_get_tracks(const char *json, CDInfo *out) {
    const char *p = json_find_value(json, "tracks");
    if (!p || *p != '[') return -1;

    /* Skip to the start of the tracks array */
    p++;

    out->track_count = 0;

    while (*p && *p != ']' && out->track_count < MAX_TRACKS) {
        const char *track_start = strchr(p, '{');
        if (!track_start || track_start > strstr(p, "]")) break;

        int depth = 1;
        int in_string = 0;
        const char *cursor = track_start + 1;
        while (*cursor && depth > 0) {
            if (*cursor == '"' && *(cursor - 1) != '\\') {
                in_string = !in_string;
            } else if (!in_string) {
                if (*cursor == '{') depth++;
                else if (*cursor == '}') depth--;
            }
            cursor++;
        }
        if (depth != 0) break;

        const char *obj_end = cursor;
        const char *title_ptr = json_find_value(track_start, "title");
        if (!title_ptr || title_ptr >= obj_end || *title_ptr != '"') break;
        title_ptr++;

        size_t i = 0;
        while (*title_ptr && *title_ptr != '"' && i < MAX_TRACK_LEN - 1) {
            out->tracks[out->track_count][i++] = *title_ptr++;
        }
        out->tracks[out->track_count][i] = '\0';
        out->track_count++;

        p = obj_end;
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
    p++;

    int depth = 1;
    int in_string = 0;
    const char *end = p;
    while (*end && depth > 0) {
        if (*end == '"' && *(end - 1) != '\\') {
            in_string = !in_string;
        } else if (!in_string) {
            if (*end == '[') depth++;
            else if (*end == ']') depth--;
        }
        end++;
    }
    if (depth != 0) return -1;

    size_t written = 0;
    const char *item = p;
    while (item < end && written + 1 < out_size) {
        const char *obj_start = strchr(item, '{');
        if (!obj_start || obj_start >= end) break;

        int obj_depth = 1;
        in_string = 0;
        const char *cursor = obj_start + 1;
        while (*cursor && obj_depth > 0 && cursor < end) {
            if (*cursor == '"' && *(cursor - 1) != '\\') {
                in_string = !in_string;
            } else if (!in_string) {
                if (*cursor == '{') obj_depth++;
                else if (*cursor == '}') obj_depth--;
            }
            cursor++;
        }
        if (obj_depth != 0) break;

        const char *obj_end = cursor;
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

        item = obj_end;
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

    if (httpResponseGetStatusCode(trans, &status_code) < 0 ||
        status_code != HTTP_STATUS_CODE_OK) {
        httpDestroyTransaction(trans);
        httpDestroyClient(client);
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
        json_get_release_str(body, "genre", out->genre, MAX_STR_LEN);
        json_get_tracks(body, out);
    }

    out->discid = id;
    out->valid  = (out->artist[0] && out->album[0] && out->track_count > 0) ? 1 : 0;
    if (!out->valid) return -1;

    mb_cache_put(id, out);
    return 0;
}

void cdinfo_to_amg_album(const CDInfo *info, AmgAlbum *album, AmgTrack *tracks) {
    if (!info || !album || !tracks) return;

    album->albumTitle = info->album;
    album->albumArtist = info->artist;
    album->albumGenre = info->genre[0] ? info->genre : "";
    album->trackCount = info->track_count;

    for (int i = 0; i < info->track_count && i < MAX_TRACKS; i++) {
        tracks[i].title = info->tracks[i];
    }
    album->tracks = tracks;
}
