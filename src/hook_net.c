#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* PSL1GHT 的 syscall 头在严格 C99 下仍使用 GNU asm 拼写。 */
#if defined(__STRICT_ANSI__)
#define asm __asm__
#endif

#include <net/socket.h>
#include <lv2/memory.h>
#include <lv2/syscalls.h>
#include <ppu-lv2.h>
#include <sys/process.h>
#include "hook_net.h"
#include "toc_reader.h"
#include "gnudb.h"
#include "formatter.h"
#include "log.h"

/* ------------------------------------------------------------------ */
/* 全局状态                                                              */
/* ------------------------------------------------------------------ */

typedef ssize_t (*recv_func_t)(int sock, void *buf, size_t len, int flags);
typedef ssize_t (*send_func_t)(int sock, const void *buf, size_t len, int flags);

/*
 * PPU 使用 ELFv1 函数描述符。普通 C 函数指针指向 OPD，而不是直接指向
 * 指令地址；sys_net 的导入桩则保存一个 32 位 OPD 地址。
 */
typedef struct {
    uint64_t entry;
    uint64_t toc;
    uint64_t environment;
} FunctionDescriptor;

typedef struct {
    uint32_t entry;
    uint32_t toc;
} FunctionDescriptor32;

extern uint32_t netRecv_stub;
extern uint32_t netSend_stub;

#define SYSCALL8_OPCODE_PS3MAPI        0x7777
#define PS3MAPI_OPCODE_SET_PROCESS_MEM 0x0032

static void *original_recv_entry = NULL;
static void *original_send_entry = NULL;

/*
 * Trampoline 必须位于可执行段；用汇编预留空间可以明确生成 AX 段，
 * 避免把普通可写数组错误地映射进不可执行数据页。
 */
__asm__(
    ".pushsection .text.trampoline,\"ax\",@progbits\n"
    ".balign 16\n"
    "recv_trampoline:\n"
    ".space 64\n"
    ".balign 16\n"
    "send_trampoline:\n"
    ".space 64\n"
    ".popsection\n"
);
extern uint32_t recv_trampoline[16];
extern uint32_t send_trampoline[16];

static uint32_t recv_original_instructions[6];
static FunctionDescriptor recv_trampoline_descriptor;

static uint32_t send_original_instructions[6];
static FunctionDescriptor send_trampoline_descriptor;

/* 当前disc ID（从CDDB请求中提取） */
static char     current_discid[9] = {0};
static char     current_toc[1024] = {0};
static uint8_t  discid_loaded = 0;
static int      current_cddb_socket = -1;

/* ------------------------------------------------------------------ */
/* 辅助函数                                                              */
/* ------------------------------------------------------------------ */

/*
 * 判断是否是CDDB请求
 * 裸协议形如 "cddb query ..."；HTTP 形式则是
 * "GET /~cddb/cddb.cgi?cmd=cddb+query+..."，标记出现在前部。
 */
static int is_cddb_request(const void *buf, ssize_t len) {
    static const char lower_query[] = "cddb query ";
    static const char upper_query[] = "CDDB query ";
    const uint8_t *bytes = (const uint8_t *)buf;
    const size_t query_len = sizeof(lower_query) - 1;

    if (!buf || len < (ssize_t)query_len) return 0;

    /* 快速路径：绝大多数数据包在首字节即可排除 */
    if (memcmp(bytes, lower_query, query_len) == 0 ||
        memcmp(bytes, upper_query, query_len) == 0) {
        return 1;
    }

    /* 兜底扫描限定在请求头部范围内（URL/query 一定在前面） */
    ssize_t scan = (len < 1024) ? len : 1024;
    for (ssize_t i = 1; i <= scan - (ssize_t)query_len; i++) {
        if (memcmp(bytes + i, lower_query, query_len) == 0 ||
            memcmp(bytes + i, upper_query, query_len) == 0) {
            return 1;
        }
    }

    return 0;
}

/*
 * 判断是否是CDDB响应
 */
static int is_cddb_response(const void *buf, ssize_t len) {
    static const char *markers[] = {
        "DTITLE=", "TTITLE0=", "210 ", "211 "
    };
    const uint8_t *bytes = (const uint8_t *)buf;

    if (!buf || len < 4) return 0;
    for (size_t marker = 0; marker < sizeof(markers) / sizeof(markers[0]);
         marker++) {
        size_t marker_len = strlen(markers[marker]);
        for (ssize_t i = 0; i <= len - (ssize_t)marker_len; i++) {
            if (memcmp(bytes + i, markers[marker], marker_len) == 0) return 1;
        }
    }
    return 0;
}

static int buffer_contains(const void *buf, ssize_t len, const char *needle) {
    const uint8_t *bytes = (const uint8_t *)buf;
    size_t needle_len;

    if (!buf || !needle || len <= 0) return 0;
    needle_len = strlen(needle);
    if (needle_len == 0 || (size_t)len < needle_len) return 0;

    for (size_t i = 0; i <= (size_t)len - needle_len; i++) {
        if (memcmp(bytes + i, needle, needle_len) == 0) return 1;
    }
    return 0;
}

static int load_disc_query_from_drive(void) {
    CdromToc toc;

    if (toc_read(&toc) < 0) return -1;
    if (toc_format_query(&toc, current_toc, sizeof(current_toc)) < 0) {
        current_toc[0] = '\0';
        return -1;
    }

    toc_discid_str(&toc, current_discid);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Hook函数                                                              */
/* ------------------------------------------------------------------ */

/*
 * 拦截send，提取disc ID
 */
static ssize_t hooked_send(int sock, const void *buf, size_t len, int flags) {
    char request[1280];

    /* 先调用原始send */
    ssize_t ret =
        ((send_func_t)&send_trampoline_descriptor)(sock, buf, len, flags);
    
    /* 检查是否是CDDB请求 */
    if (ret > 0 && is_cddb_request(buf, ret)) {
        size_t request_size = (size_t)ret;
        if (request_size >= sizeof(request)) request_size = sizeof(request) - 1;
        memcpy(request, buf, request_size);
        request[request_size] = '\0';

        discid_loaded = 0;
        memset(current_discid, 0, sizeof(current_discid));
        memset(current_toc, 0, sizeof(current_toc));

        /*
         * 优先读取物理光驱；CFW 未开放 sys_storage 或 ATAPI 读取失败时，
         * 回退到 XMB 自己生成的 CDDB 请求参数。
         */
        if (load_disc_query_from_drive() == 0 ||
            parse_cddb_query(request, current_discid,
                             sizeof(current_discid), current_toc,
                             sizeof(current_toc)) == 0) {
            discid_loaded = 1;
            current_cddb_socket = sock;
            plugin_log("CDDB request intercepted: discid=%s sock=%d",
                       current_discid, sock);
        } else {
            plugin_log("CDDB request but TOC unavailable (drive read + "
                       "request parse both failed)");
        }
    }
    
    return ret;
}

/*
 * 拦截recv，替换CDDB响应
 */
static ssize_t hooked_recv(int sock, void *buf, size_t len, int flags) {
    /* 先调用原始recv */
    ssize_t ret =
        ((recv_func_t)&recv_trampoline_descriptor)(sock, buf, len, flags);

    if (ret <= 0) return ret;
    if (sock != current_cddb_socket) return ret;
    /* 廉价检查放前面，避免对无关数据做全文标记扫描 */
    if (!discid_loaded || !current_discid[0]) return ret;
    if (!is_cddb_response(buf, ret)) return ret;
    if (!buffer_contains(buf, ret, ".\r\n")) return ret;

    /* 查询gnudb */
    CDInfo info;
    if (gnudb_query(current_discid, current_toc, &info) < 0) {
        plugin_log("gnudb query failed for discid=%s (kept original response)",
                   current_discid);
        return ret;
    }

    /* 格式化CDDB响应 */
    char response[4096];
    int response_len = format_cddb_response(response, sizeof(response), &info);
    if (response_len < 0 || response_len > (int)len) {
        plugin_log("format response failed or too large (len=%d)", response_len);
        return ret;
    }

    memcpy(buf, response, response_len);

    plugin_log("replaced CDDB response: %s - %s (%d tracks)",
               info.artist, info.album, info.track_count);

    discid_loaded = 0;
    current_cddb_socket = -1;
    current_discid[0] = '\0';
    current_toc[0] = '\0';

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

/*
 * 从共享 sys_net 导出跳入本插件时，r2 仍是 sys_net 的 TOC。
 * 先切换到 hooked_* 所属模块的 TOC，再跳转到其真实代码地址。
 */
static void ppc_load_toc_and_jump(uint32_t *code, void *target,
                                  uint64_t toc) {
    uint64_t addr = (uint64_t)target;

    code[0] = 0x3C400000 | ((toc >> 16) & 0xFFFF);  /* lis r2, toc@h */
    code[1] = 0x60420000 | (toc & 0xFFFF);          /* ori r2,r2,toc@l */
    code[2] = 0x3D800000 | ((addr >> 16) & 0xFFFF); /* lis r12, fn@h */
    code[3] = 0x618C0000 | (addr & 0xFFFF);         /* ori r12,r12,fn@l */
    code[4] = 0x7D8903A6;                           /* mtctr r12 */
    code[5] = 0x4E800420;                           /* bctr */
}

static void flush_instruction_cache(void *address, size_t size) {
#ifndef HOST_TEST
    uint8_t *p = (uint8_t *)address;

    for (size_t offset = 0; offset < size; offset += sizeof(uint32_t)) {
        __asm__ __volatile__("dcbst 0, %0" : : "r"(p + offset) : "memory");
    }
    __asm__ __volatile__("sync" : : : "memory");
    for (size_t offset = 0; offset < size; offset += sizeof(uint32_t)) {
        __asm__ __volatile__("icbi 0, %0" : : "r"(p + offset) : "memory");
    }
    __asm__ __volatile__("isync" : : : "memory");
#else
    (void)address;
    (void)size;
#endif
}

static int resolve_net_export(uint32_t stub,
                              void **entry_out, uint64_t *toc_out) {
    const FunctionDescriptor32 *descriptor;

    if (!stub || !entry_out || !toc_out) return -1;
    descriptor = (const FunctionDescriptor32 *)(uintptr_t)stub;
    if (!descriptor->entry || !descriptor->toc) return -1;

    *entry_out = (void *)(uintptr_t)descriptor->entry;
    *toc_out = descriptor->toc;
    return 0;
}

static int write_process_memory(void *address, const void *data, size_t size) {
    s32 result;

    /*
     * Cobra/Mamba/PS3HEN 的 PS3MAPI set-process-memory 能写 RX 页面。
     * boot_plugins.txt 本身也依赖这类 payload，因此不可用时直接拒绝
     * 安装 hook，避免普通 memcpy 触发 VSH 崩溃。
     */
    lv2syscall6(8,
                SYSCALL8_OPCODE_PS3MAPI,
                PS3MAPI_OPCODE_SET_PROCESS_MEM,
                (u64)(u32)sysProcessGetPid(),
                (u64)address,
                (u64)data,
                (u64)size);
    result = (s32)p1;
    if (result != 0) return -1;

    if (memcmp(address, data, size) != 0) return -1;
    flush_instruction_cache(address, size);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Hook安装/卸载                                                          */
/* ------------------------------------------------------------------ */

/*
 * 安装hook
 */
int hook_net_init(void) {
    const FunctionDescriptor *hooked_recv_descriptor =
        (const FunctionDescriptor *)(void *)hooked_recv;
    const FunctionDescriptor *hooked_send_descriptor =
        (const FunctionDescriptor *)(void *)hooked_send;
    uint64_t recv_toc;
    uint64_t send_toc;
    uint32_t recv_hook_jump[6];
    uint32_t send_hook_jump[6];

    if (resolve_net_export(netRecv_stub, &original_recv_entry, &recv_toc) < 0 ||
        resolve_net_export(netSend_stub, &original_send_entry, &send_toc) < 0) {
        original_recv_entry = NULL;
        original_send_entry = NULL;
        plugin_log("ERROR: failed to resolve sys_net recv/send export");
        return -1;
    }
    if ((hooked_recv_descriptor->entry >> 32) != 0 ||
        (hooked_recv_descriptor->toc >> 32) != 0 ||
        (hooked_send_descriptor->entry >> 32) != 0 ||
        (hooked_send_descriptor->toc >> 32) != 0) {
        original_recv_entry = NULL;
        original_send_entry = NULL;
        plugin_log("ERROR: hook descriptor address exceeds 32-bit");
        return -1;
    }

    /* ===== Hook recv ===== */
    
    /* 保存recv原始指令 */
    memcpy(recv_original_instructions, original_recv_entry,
           sizeof(recv_original_instructions));
    
    /* 构造recv trampoline */
    uint32_t recv_trampoline_code[10];
    uint32_t send_trampoline_code[10];

    memcpy(recv_trampoline_code, recv_original_instructions,
           sizeof(recv_original_instructions));
    ppc_load_and_jump(&recv_trampoline_code[6],
                      (char *)original_recv_entry +
                      sizeof(recv_original_instructions));
    if (write_process_memory(recv_trampoline, recv_trampoline_code,
                             sizeof(recv_trampoline_code)) < 0) {
        original_recv_entry = NULL;
        original_send_entry = NULL;
        plugin_log("ERROR: PS3MAPI write failed (recv trampoline)");
        return -1;
    }
    recv_trampoline_descriptor.entry = (uint64_t)recv_trampoline;
    recv_trampoline_descriptor.toc = recv_toc;
    recv_trampoline_descriptor.environment = 0;
    
    /* 修改recv开头跳转到hooked_recv */
    ppc_load_toc_and_jump(recv_hook_jump,
                          (void *)(uintptr_t)hooked_recv_descriptor->entry,
                          hooked_recv_descriptor->toc);
    if (write_process_memory(original_recv_entry, recv_hook_jump,
                             sizeof(recv_hook_jump)) < 0) {
        original_recv_entry = NULL;
        original_send_entry = NULL;
        plugin_log("ERROR: PS3MAPI write failed (recv prologue)");
        return -1;
    }

    /* ===== Hook send ===== */
    
    /* 保存send原始指令 */
    memcpy(send_original_instructions, original_send_entry,
           sizeof(send_original_instructions));
    
    /* 构造send trampoline */
    memcpy(send_trampoline_code, send_original_instructions,
           sizeof(send_original_instructions));
    ppc_load_and_jump(&send_trampoline_code[6],
                      (char *)original_send_entry +
                      sizeof(send_original_instructions));
    if (write_process_memory(send_trampoline, send_trampoline_code,
                             sizeof(send_trampoline_code)) < 0) {
        write_process_memory(original_recv_entry, recv_original_instructions,
                             sizeof(recv_original_instructions));
        original_recv_entry = NULL;
        original_send_entry = NULL;
        plugin_log("ERROR: PS3MAPI write failed (send trampoline)");
        return -1;
    }
    send_trampoline_descriptor.entry = (uint64_t)send_trampoline;
    send_trampoline_descriptor.toc = send_toc;
    send_trampoline_descriptor.environment = 0;
    
    /* 修改send开头跳转到hooked_send */
    ppc_load_toc_and_jump(send_hook_jump,
                          (void *)(uintptr_t)hooked_send_descriptor->entry,
                          hooked_send_descriptor->toc);
    if (write_process_memory(original_send_entry, send_hook_jump,
                             sizeof(send_hook_jump)) < 0) {
        write_process_memory(original_recv_entry, recv_original_instructions,
                             sizeof(recv_original_instructions));
        original_recv_entry = NULL;
        original_send_entry = NULL;
        plugin_log("ERROR: PS3MAPI write failed (send prologue)");
        return -1;
    }

    plugin_log("network hook installed (recv=%p send=%p)",
               original_recv_entry, original_send_entry);
    return 0;
}

/*
 * 卸载hook
 */
void hook_net_cleanup(void) {
    if (original_recv_entry) {
        /* 恢复recv */
        write_process_memory(original_recv_entry, recv_original_instructions,
                             sizeof(recv_original_instructions));
        original_recv_entry = NULL;
    }
    
    if (original_send_entry) {
        /* 恢复send */
        write_process_memory(original_send_entry, send_original_instructions,
                             sizeof(send_original_instructions));
        original_send_entry = NULL;
    }

    discid_loaded = 0;
    current_cddb_socket = -1;
    memset(current_discid, 0, sizeof(current_discid));
    memset(current_toc, 0, sizeof(current_toc));
}
