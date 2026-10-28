#include <stdio.h>
#include <string.h>
#include <assert.h>
#include <stdint.h>

int parse_cddb_query(const char *request,
                     char *discid_out, size_t discid_out_size,
                     char *toc_out, size_t toc_out_size);

int httpClientSetUserAgent(int client, const char *agent) { (void)client; (void)agent; return 0; }
int httpCreateClient(int *client) { (void)client; return -1; }
int httpCreateTransaction(int *trans, int client, int method, void *uri) { (void)trans; (void)client; (void)method; (void)uri; return -1; }
int httpDestroyClient(int client) { (void)client; return 0; }
int httpDestroyTransaction(int trans) { (void)trans; return 0; }
int httpRequestAddHeader(int trans, const void *header) { (void)trans; (void)header; return 0; }
int httpSendRequest(int trans, const void *buf, size_t size, uint32_t *sent) { (void)trans; (void)buf; (void)size; (void)sent; return 0; }
int httpRecvResponse(int trans, void *buf, size_t size, uint32_t *recvd) { (void)trans; (void)buf; (void)size; (void)recvd; return -1; }
int httpUtilParseUri(void *uri, const char *url, void *pool, size_t pool_size, uint32_t *required) { (void)uri; (void)url; (void)pool; (void)pool_size; (void)required; return 0; }
int httpInit(void *pool, size_t pool_size) { (void)pool; (void)pool_size; return 0; }
int sysModuleLoad(int module_id) { (void)module_id; return 0; }

int main(void) {
    const char *request = "cddb query a50d4a0c 13 150 15000 28000 42000 58000 72000 88000 103000 118000 135000 152000 168000 185000 200000\r\n";

    char discid[16];
    char toc[256];

    assert(parse_cddb_query(request, discid, sizeof(discid), toc, sizeof(toc)) == 0);
    assert(strcmp(discid, "a50d4a0c") == 0);
    /*
     * CDDB 请求最后一个字段是总秒数（200000），还原为绝对帧：
     * leadout = (200000 + 150/75) * 75 = 15000150
     * MusicBrainz fuzzy TOC 的 leadout 必须是帧而不是秒。
     */
    assert(strcmp(toc, "1+13+15000150+150+15000+28000+42000+58000+72000+88000+103000+118000+135000+152000+168000+185000") == 0);

    printf("✅ parse_cddb_query works: discid=%s toc=%s\n", discid, toc);
    return 0;
}
