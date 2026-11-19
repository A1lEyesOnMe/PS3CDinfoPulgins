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
#include "musicbrainz.h"
#include "formatter.h"
#include "amg_response.h"
#include "debug.h"

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
 * 格式: "cddb query DISCID NTRACKS ..."
 */
static int is_cddb_request(const void *buf, ssize_t len) {
    static const char lower_query[] = "cddb query ";
    static const char upper_query[] = "CDDB query ";
    const uint8_t *bytes = (const uint8_t *)buf;

    if (!buf || len < (ssize_t)(sizeof(lower_query) - 1)) return 0;

    for (ssize_t i = 0;
         i <= len - (ssize_t)(sizeof(lower_query) - 1);
         i++) {
        if (memcmp(bytes + i, lower_query, sizeof(lower_query) - 1) == 0 ||
            memcmp(bytes + i, upper_query, sizeof(upper_query) - 1) == 0) {
            return 1;
        }
    }

    return 0;
}

/*
 * 从CDDB请求中提取disc ID
 * 格式: "cddb query a50d4a0c 13 150 15000 ..."
 */
int parse_cddb_query(const char *request, char *discid_out,
                             size_t discid_out_size,
                             char *toc_out, size_t toc_out_size) {
    if (!request || !discid_out || discid_out_size < 9 ||
        !toc_out || toc_out_size == 0) {
        return -1;
    }

    const char *p = strstr(request, "cddb query ");
    if (!p) p = strstr(request, "CDDB query ");
    if (!p) return -1;

    p += strlen("cddb query ");
    while (*p == ' ') p++;

    size_t i = 0;
    while (*p && *p != ' ' && *p != '\r' && *p != '\n' && i < discid_out_size - 1) {
        discid_out[i++] = *p++;
    }
    discid_out[i] = '\0';
    if (i != 8) return -1;
    for (size_t j = 0; j < 8; j++) {
        char c = discid_out[j];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
              (c >= 'A' && c <= 'F'))) {
            return -1;
        }
    }

    while (*p == ' ') p++;
    char *endptr;
    long track_count = strtol(p, &endptr, 10);
    if (track_count <= 0 || track_count > MAX_TRACKS) return -1;
    p = endptr;

    uint32_t offsets[MAX_TRACKS] = {0};
    for (int idx = 0; idx < track_count; idx++) {
        while (*p == ' ') p++;
        if (!*p || *p == '\r' || *p == '\n') return -1;
        char *next;
        long offset = strtol(p, &next, 10);
        if (next == p || offset < 0 || (uint64_t)offset > UINT32_MAX) {
            return -1;
        }
        if (idx > 0 && (uint32_t)offset <= offsets[idx - 1]) return -1;
        offsets[idx] = (uint32_t)offset;
        p = next;
    }

    /*
     * CDDB 的最后一个字段是从第一轨到 lead-out 的秒数，不是帧偏移。
     * fuzzy TOC 需要绝对帧，因此按 FreeDB 的 disc length 定义还原。
     */
    while (*p == ' ') p++;
    char *next;
    long total_seconds = strtol(p, &next, 10);
    if (next == p || total_seconds <= 0 ||
        (uint64_t)total_seconds > UINT32_MAX / 75U) {
        return -1;
    }
    uint64_t leadout = ((uint64_t)total_seconds +
                        offsets[0] / 75U) * 75U;
    if (leadout > UINT32_MAX ||
        leadout <= offsets[track_count - 1]) {
        return -1;
    }

    int written = snprintf(toc_out, toc_out_size, "1+%ld+%u",
                           track_count, (uint32_t)leadout);
    if (written < 0 || (size_t)written >= toc_out_size) return -1;

    for (int idx = 0; idx < track_count; idx++) {
        size_t remaining = toc_out_size - (size_t)written;
        int appended = snprintf(toc_out + written, remaining, "+%u",
                                offsets[idx]);
        if (appended < 0 || (size_t)appended >= remaining) return -1;
        written += appended;
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

        debug_log(DEBUG_LEVEL_VERBOSE, "CDDB request intercepted on socket %d", sock);
        debug_notify("CD detected! Reading TOC...");

        /*
         * 优先读取物理光驱；CFW 未开放 sys_storage 或 ATAPI 读取失败时，
         * 回退到 XMB 自己生成的 CDDB 请求参数。
         */
        if (load_disc_query_from_drive() == 0) {
            debug_log(DEBUG_LEVEL_INFO, "TOC read from drive: %s", current_discid);
            debug_notify("TOC read: %s", current_discid);
            discid_loaded = 1;
            current_cddb_socket = sock;
        } else if (parse_cddb_query(request, current_discid,
                                    sizeof(current_discid), current_toc,
                                    sizeof(current_toc)) == 0) {
            debug_log(DEBUG_LEVEL_INFO, "TOC parsed from CDDB query: %s", current_discid);
            debug_notify("TOC from query: %s", current_discid);
            discid_loaded = 1;
            current_cddb_socket = sock;
        } else {
            debug_log(DEBUG_LEVEL_ERROR, "Failed to load TOC");
            debug_notify("CD Info: TOC read FAILED!");
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
    if (!is_cddb_response(buf, ret)) return ret;
    if (!buffer_contains(buf, ret, ".\r\n")) return ret;

    /* 检查是否已获取disc ID */
    if (!discid_loaded || !current_discid[0]) {
        return ret;  /* 没有disc ID，返回原始数据 */
    }

    /* 查询MusicBrainz */
    CDInfo info;
    if (mb_query(current_discid, current_toc, &info) < 0) {
        debug_log(DEBUG_LEVEL_ERROR, "MusicBrainz query failed for %s", current_discid);
        debug_notify("CD Info: MB query failed");
        return ret;
    }

    debug_log(DEBUG_LEVEL_INFO, "MB query success: %s - %s (%d tracks)", 
              info.artist, info.album, info.track_count);
    debug_notify("MB: %s - %s", info.artist, info.album);

    /*
     * 构建AMG二进制响应（XMB原生格式）
     * 这是simple-cd-info项目的关键发现：XMB期望的是AMG二进制格式，而非CDDB文本。
     * AMG格式是PS3固件x3_amgsdk模块原生解析的格式。
     *
     * 注意：PS3 XMB是单线程的，静态缓冲区是安全的。如果未来需要多线程支持，
     * 应改为线程本地存储或动态分配。
     *
     * 回退策略：
     * 1. 优先尝试AMG二进制格式（XMB原生）
     * 2. AMG失败时回退到CDDB文本格式（向后兼容）
     * 3. 两者都失败时返回原始数据（不破坏功能）
     */
    static unsigned char amg_scratch[AMG_MIN_SCRATCH_SIZE];
    char response[AMG_RECOMMENDED_BUF_SIZE];
    int response_len = format_amg_response(response, sizeof(response), &info,
                                           amg_scratch, sizeof(amg_scratch));
    if (response_len > 0 && response_len <= (int)len) {
        debug_log(DEBUG_LEVEL_INFO, "AMG response built: %d bytes", response_len);
        debug_notify("AMG built: %d bytes", response_len);
    } else {
        debug_log(DEBUG_LEVEL_VERBOSE, "AMG build failed, trying CDDB");
        /* AMG格式构建失败，回退到CDDB文本格式 */
        response_len = format_cddb_response(response, sizeof(response), &info);
        if (response_len < 0 || response_len > (int)len) {
            /* CDDB也失败，返回原始数据 */
            debug_log(DEBUG_LEVEL_ERROR, "Both AMG and CDDB failed");
            debug_notify("CD Info: Format FAILED!");
            return ret;
        }
        debug_log(DEBUG_LEVEL_INFO, "CDDB fallback used: %d bytes", response_len);
        debug_notify("CDDB fallback: %d bytes", response_len);
    }

    /* 替换内容并验证 */
    memcpy(buf, response, response_len);
    if (memcmp(buf, response, response_len) != 0) {
        return ret;
    }

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
        return -1;
    }
    if ((hooked_recv_descriptor->entry >> 32) != 0 ||
        (hooked_recv_descriptor->toc >> 32) != 0 ||
        (hooked_send_descriptor->entry >> 32) != 0 ||
        (hooked_send_descriptor->toc >> 32) != 0) {
        original_recv_entry = NULL;
        original_send_entry = NULL;
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
        return -1;
    }

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
