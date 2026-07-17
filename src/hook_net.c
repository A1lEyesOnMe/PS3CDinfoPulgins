#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <net/socket.h>
#include <lv2/memory.h>
#include <lv2/syscalls.h>
#include "hook_net.h"
#include "toc_reader.h"
#include "musicbrainz.h"
#include "formatter.h"

/* ------------------------------------------------------------------ */
/* 全局状态                                                              */
/* ------------------------------------------------------------------ */

typedef ssize_t (*recv_func_t)(int sock, void *buf, size_t len, int flags);
typedef ssize_t (*send_func_t)(int sock, const void *buf, size_t len, int flags);

static recv_func_t original_recv = NULL;
static send_func_t original_send = NULL;

/* Trampoline：保存原函数的前几条指令 */
static uint32_t recv_trampoline[16];
static uint32_t recv_original_instructions[4];

static uint32_t send_trampoline[16];
static uint32_t send_original_instructions[4];

/* 当前disc ID（从CDDB请求中提取） */
static char     current_discid[9] = {0};
static uint8_t  discid_loaded = 0;

/* ------------------------------------------------------------------ */
/* 辅助函数                                                              */
/* ------------------------------------------------------------------ */

/*
 * 判断是否是CDDB请求
 * 格式: "cddb query DISCID NTRACKS ..."
 */
static int is_cddb_request(const void *buf, ssize_t len) {
    if (len < 20 || !buf) return 0;
    const char *str = (const char *)buf;
    
    if (strstr(str, "cddb query ")) return 1;
    if (strstr(str, "CDDB query ")) return 1;
    
    return 0;
}

/*
 * 从CDDB请求中提取disc ID
 * 格式: "cddb query a50d4a0c 13 150 15000 ..."
 */
static int parse_cddb_discid(const char *request, char *out, size_t out_size) {
    /* 查找 "cddb query " 或 "CDDB query " */
    const char *p = strstr(request, "cddb query ");
    if (!p) p = strstr(request, "CDDB query ");
    if (!p) return -1;
    
    p += strlen("cddb query ");
    
    /* 跳过空格 */
    while (*p == ' ') p++;
    
    /* 提取disc ID（8位hex字符） */
    size_t i = 0;
    while (*p && *p != ' ' && *p != '\r' && *p != '\n' && i < out_size - 1) {
        out[i++] = *p++;
    }
    out[i] = '\0';
    
    /* 验证是8位hex */
    if (i != 8) return -1;
    for (size_t j = 0; j < 8; j++) {
        char c = out[j];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) {
            return -1;
        }
    }
    
    return 0;
}

/*
 * 判断是否是CDDB响应
 */
static int is_cddb_response(const void *buf, ssize_t len) {
    if (len < 10 || !buf) return 0;
    const char *str = (const char *)buf;

    if (strstr(str, "DTITLE=") || strstr(str, "TTITLE0=")) return 1;
    if (strstr(str, "210 ") || strstr(str, "211 ")) return 1;
    
    return 0;
}

/* ------------------------------------------------------------------ */
/* Hook函数                                                              */
/* ------------------------------------------------------------------ */

/*
 * 拦截send，提取disc ID
 */
static ssize_t hooked_send(int sock, const void *buf, size_t len, int flags) {
    /* 先调用原始send */
    ssize_t ret = ((send_func_t)send_trampoline)(sock, buf, len, flags);
    
    /* 检查是否是CDDB请求 */
    if (ret > 0 && is_cddb_request(buf, ret)) {
        /* 提取disc ID并缓存 */
        if (parse_cddb_discid((const char *)buf, current_discid, sizeof(current_discid)) == 0) {
            discid_loaded = 1;
        }
    }
    
    return ret;
}

/*
 * 拦截recv，替换CDDB响应
 */
static ssize_t hooked_recv(int sock, void *buf, size_t len, int flags) {
    /* 先调用原始recv */
    ssize_t ret = ((recv_func_t)recv_trampoline)(sock, buf, len, flags);

    if (ret <= 0) return ret;
    if (!is_cddb_response(buf, ret)) return ret;

    /* 检查是否已获取disc ID */
    if (!discid_loaded || !current_discid[0]) {
        return ret;  /* 没有disc ID，返回原始数据 */
    }

    /* 查询MusicBrainz */
    CDInfo info;
    if (mb_query(current_discid, &info) < 0) {
        return ret;
    }

    /* 格式化CDDB响应 */
    char response[4096];
    int response_len = format_cddb_response(response, sizeof(response), &info);
    if (response_len < 0 || response_len > (int)len) {
        return ret;
    }

    /* 替换内容并验证 */
    memcpy(buf, response, response_len);
    if (memcmp(buf, response, response_len) != 0) {
        return ret;
    }

    return (ssize_t)response_len;
}

/* ------------------------------------------------------------------ */
/* PowerPC指令编码                                                        */
/* ------------------------------------------------------------------ */

/*
 * 生成加载立即数到寄存器并跳转的指令序列
 * lis r12, hi
 * ori r12, r12, lo
 * mtctr r12
 * bctr
 */
static void ppc_load_and_jump(uint32_t *code, void *target) {
    uint64_t addr = (uint64_t)target;
    uint32_t hi = (addr >> 16) & 0xFFFF;
    uint32_t lo = addr & 0xFFFF;
    
    code[0] = 0x3D800000 | hi;        /* lis r12, hi */
    code[1] = 0x618C0000 | lo;        /* ori r12, r12, lo */
    code[2] = 0x7D8903A6;             /* mtctr r12 */
    code[3] = 0x4E800420;             /* bctr */
}

/* ------------------------------------------------------------------ */
/* Hook安装/卸载                                                          */
/* ------------------------------------------------------------------ */

/*
 * 安装hook
 */
int hook_net_init(void) {
    void *recv_addr = (void *)recv;
    void *send_addr = (void *)send;
    
    original_recv = (recv_func_t)recv_addr;
    original_send = (send_func_t)send_addr;

    /* ===== Hook recv ===== */
    
    /* 保存recv原始指令 */
    memcpy(recv_original_instructions, recv_addr, 16);
    
    /* 构造recv trampoline */
    memcpy(recv_trampoline, recv_original_instructions, 16);
    ppc_load_and_jump(&recv_trampoline[4], (char *)recv_addr + 16);
    
    /* 修改recv开头跳转到hooked_recv */
    uint32_t recv_hook_jump[4];
    ppc_load_and_jump(recv_hook_jump, (void *)hooked_recv);
    memcpy(recv_addr, recv_hook_jump, 16);
    
    /* 刷新recv的指令缓存 */
    __asm__ __volatile__(
        "dcbst 0, %0\n"
        "sync\n"
        "icbi 0, %0\n"
        "isync\n"
        : : "r"(recv_addr)
    );

    /* ===== Hook send ===== */
    
    /* 保存send原始指令 */
    memcpy(send_original_instructions, send_addr, 16);
    
    /* 构造send trampoline */
    memcpy(send_trampoline, send_original_instructions, 16);
    ppc_load_and_jump(&send_trampoline[4], (char *)send_addr + 16);
    
    /* 修改send开头跳转到hooked_send */
    uint32_t send_hook_jump[4];
    ppc_load_and_jump(send_hook_jump, (void *)hooked_send);
    memcpy(send_addr, send_hook_jump, 16);
    
    /* 刷新send的指令缓存 */
    __asm__ __volatile__(
        "dcbst 0, %0\n"
        "sync\n"
        "icbi 0, %0\n"
        "isync\n"
        : : "r"(send_addr)
    );

    return 0;
}

/*
 * 卸载hook
 */
void hook_net_cleanup(void) {
    if (original_recv) {
        /* 恢复recv */
        void *recv_addr = (void *)original_recv;
        memcpy(recv_addr, recv_original_instructions, 16);
        
        __asm__ __volatile__(
            "dcbst 0, %0\n"
            "sync\n"
            "icbi 0, %0\n"
            "isync\n"
            : : "r"(recv_addr)
        );
        
        original_recv = NULL;
    }
    
    if (original_send) {
        /* 恢复send */
        void *send_addr = (void *)original_send;
        memcpy(send_addr, send_original_instructions, 16);
        
        __asm__ __volatile__(
            "dcbst 0, %0\n"
            "sync\n"
            "icbi 0, %0\n"
            "isync\n"
            : : "r"(send_addr)
        );
        
        original_send = NULL;
    }

    discid_loaded = 0;
    memset(current_discid, 0, sizeof(current_discid));
}