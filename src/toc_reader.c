#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include <ppu-lv2.h>
#include <ppu-types.h>
#include <lv2/syscalls.h>
#include "toc_reader.h"

/*
 * PSL1GHT 的 lv2syscall 宏使用 GNU 的 asm 拼写；本项目以严格 C99
 * 编译，因此在展开这些宏时将它映射到同义的 __asm__。
 */
#if defined(__STRICT_ANSI__)
#define asm __asm__
#define TOC_READER_UNDEFINE_ASM
#endif

/* PS3 GameOS 中的物理 Blu-ray/DVD/CD 光驱。 */
#define BDVD_DEVICE_ID UINT64_C(0x101000000000006)

/* MMC READ TOC/PMA/ATIP 命令及 LV2 ATAPI 传输参数。 */
#define ATAPI_READ_TOC              0x43
#define ATAPI_COMMAND_PACKET_LENGTH 12
#define ATAPI_PIO_DATA_IN           1
#define ATAPI_DIRECTION_READ        1
#define LV2_STORAGE_ATAPI_COMMAND   1

/*
 * 格式 0 的 TOC 最多包含 99 条轨道和一个 lead-out 描述符：
 * 4 字节响应头 + 100 个 8 字节描述符。
 */
#define TOC_HEADER_SIZE     4
#define TOC_DESCRIPTOR_SIZE 8
#define TOC_RESPONSE_SIZE \
    (TOC_HEADER_SIZE + (MAX_TRACKS + 1) * TOC_DESCRIPTOR_SIZE)
#define TOC_LEADOUT_TRACK 0xAA

/*
 * sys_storage_send_device_command() 的 ATAPI 命令块。
 * 该布局由 PS3 LV2 storage 接口定义，不能改变字段顺序或对齐。
 */
typedef struct {
    uint8_t  packet[32];
    uint32_t packet_length;
    uint32_t blocks;
    uint32_t block_size;
    uint32_t protocol;
    uint32_t direction;
    uint32_t unknown;
} __attribute__((packed)) Lv2AtapiCommand;

static s32 storage_open(uint64_t device_id, s32 *fd) {
    lv2syscall2(SYSCALL_STORAGE_OPEN, device_id, (u64)fd);
    return_to_user_prog(s32);
}

static s32 storage_close(s32 fd) {
    lv2syscall1(SYSCALL_STORAGE_CLOSE, fd);
    return_to_user_prog(s32);
}

static s32 storage_send_atapi(s32 fd, Lv2AtapiCommand *command,
                              void *buffer) {
    uint64_t tag = 0;
    uint64_t transfer_size;

    transfer_size = (uint64_t)command->blocks * command->block_size;

    lv2syscall7(SYSCALL_STORAGE_SEND_DEVICE_COMMAND,
                (u64)(u32)fd,
                LV2_STORAGE_ATAPI_COMMAND,
                (u64)command,
                sizeof(*command),
                (u64)buffer,
                transfer_size,
                (u64)&tag);
    return_to_user_prog(s32);
}

#ifdef TOC_READER_UNDEFINE_ASM
#undef asm
#undef TOC_READER_UNDEFINE_ASM
#endif

static uint32_t msf_to_frames(uint8_t minute, uint8_t second,
                              uint8_t frame) {
    return ((uint32_t)minute * 60U + (uint32_t)second) * 75U
         + (uint32_t)frame;
}

/*
 * 解析 MMC 格式 0、MSF 地址形式的 READ TOC 响应。
 *
 * 响应头：
 *   [0..1] 数据长度（不含这两个字节）
 *   [2]    第一轨编号
 *   [3]    最后一轨编号
 *
 * 每个 8 字节描述符：
 *   [2]    轨道编号（0xAA 表示 lead-out）
 *   [5..7] minute / second / frame
 */
static int parse_toc_response(const uint8_t *response, size_t response_size,
                              CdromToc *toc) {
    CdromToc parsed;
    uint8_t found[MAX_TRACKS + 1];
    uint16_t data_length;
    size_t total_length;
    size_t descriptor_count;
    int leadout_found = 0;

    if (!response || !toc || response_size < TOC_HEADER_SIZE) return -1;

    data_length = ((uint16_t)response[0] << 8) | response[1];
    total_length = (size_t)data_length + 2U;
    if (total_length < TOC_HEADER_SIZE || total_length > response_size) {
        return -1;
    }
    if ((total_length - TOC_HEADER_SIZE) % TOC_DESCRIPTOR_SIZE != 0) {
        return -1;
    }

    memset(&parsed, 0, sizeof(parsed));
    memset(found, 0, sizeof(found));

    parsed.first_track = response[2];
    parsed.last_track = response[3];
    if (parsed.first_track == 0 ||
        parsed.first_track > parsed.last_track ||
        parsed.last_track > MAX_TRACKS) {
        return -1;
    }

    descriptor_count = (total_length - TOC_HEADER_SIZE) /
                       TOC_DESCRIPTOR_SIZE;
    for (size_t i = 0; i < descriptor_count; i++) {
        const uint8_t *descriptor =
            response + TOC_HEADER_SIZE + i * TOC_DESCRIPTOR_SIZE;
        uint8_t track_number = descriptor[2];
        uint8_t minute = descriptor[5];
        uint8_t second = descriptor[6];
        uint8_t frame = descriptor[7];
        uint32_t offset;

        if (second >= 60 || frame >= 75) return -1;
        offset = msf_to_frames(minute, second, frame);

        if (track_number == TOC_LEADOUT_TRACK) {
            parsed.leadout = offset;
            leadout_found = 1;
        } else if (track_number >= parsed.first_track &&
                   track_number <= parsed.last_track) {
            parsed.tracks[track_number].minute = minute;
            parsed.tracks[track_number].second = second;
            parsed.tracks[track_number].frame = frame;
            parsed.tracks[track_number].offset = offset;
            found[track_number] = 1;
        }
    }

    if (!leadout_found) return -1;

    for (int track = parsed.first_track; track <= parsed.last_track; track++) {
        if (!found[track]) return -1;
        if (track > parsed.first_track &&
            parsed.tracks[track].offset <= parsed.tracks[track - 1].offset) {
            return -1;
        }
    }

    if (parsed.leadout <= parsed.tracks[parsed.last_track].offset) return -1;

    memcpy(toc, &parsed, sizeof(*toc));
    return 0;
}

int toc_read(CdromToc *toc) {
    /*
     * LV2 的 storage/ATAPI 缓冲区使用 128 字节对齐，避免在不同固件上
     * 因 DMA 对齐要求导致命令失败。
     */
    uint8_t response[TOC_RESPONSE_SIZE] __attribute__((aligned(128)));
    Lv2AtapiCommand command __attribute__((aligned(128)));
    s32 fd = -1;
    s32 result;

    if (!toc) return -1;
    memset(toc, 0, sizeof(*toc));
    memset(response, 0, sizeof(response));
    memset(&command, 0, sizeof(command));

    result = storage_open(BDVD_DEVICE_ID, &fd);
    if (result != 0) return -1;

    command.packet_length = ATAPI_COMMAND_PACKET_LENGTH;
    /*
     * LV2 把传输长度定义为 blocks * block_size。ATAPI 非扇区命令使用
     * 1 字节块，blocks 即返回缓冲区长度。
     */
    command.blocks = sizeof(response);
    command.block_size = 1;
    command.protocol = ATAPI_PIO_DATA_IN;
    command.direction = ATAPI_DIRECTION_READ;

    command.packet[0] = ATAPI_READ_TOC;
    command.packet[1] = 0x02; /* MSF=1，返回 minute/second/frame 地址。 */
    command.packet[2] = 0x00; /* Format 0：标准 TOC。 */
    command.packet[6] = 0x00; /* 从第一条可用轨道开始。 */
    command.packet[7] = (uint8_t)(sizeof(response) >> 8);
    command.packet[8] = (uint8_t)(sizeof(response) & 0xFF);

    result = storage_send_atapi(fd, &command, response);
    storage_close(fd);
    if (result != 0) return -1;

    return parse_toc_response(response, sizeof(response), toc);
}

int toc_format_query(const CdromToc *toc, char *out, size_t out_size) {
    int written;

    if (!toc || !out || out_size == 0) return -1;
    out[0] = '\0';

    if (toc->first_track == 0 ||
        toc->last_track < toc->first_track ||
        toc->last_track > MAX_TRACKS ||
        toc->leadout <= toc->tracks[toc->last_track].offset) {
        return -1;
    }

    written = snprintf(out, out_size, "%u+%u+%u",
                       (unsigned int)toc->first_track,
                       (unsigned int)toc->last_track,
                       (unsigned int)toc->leadout);
    if (written < 0 || (size_t)written >= out_size) return -1;

    for (int track = toc->first_track; track <= toc->last_track; track++) {
        size_t remaining;
        int appended;

        if (track > toc->first_track &&
            toc->tracks[track].offset <= toc->tracks[track - 1].offset) {
            out[0] = '\0';
            return -1;
        }

        remaining = out_size - (size_t)written;
        appended = snprintf(out + written, remaining, "+%u",
                            (unsigned int)toc->tracks[track].offset);
        if (appended < 0 || (size_t)appended >= remaining) {
            out[0] = '\0';
            return -1;
        }
        written += appended;
    }

    return written;
}

/* FreeDB校验和辅助函数 */
static uint32_t cddb_sum(uint32_t n) {
    uint32_t ret = 0;
    while (n > 0) {
        ret += n % 10;
        n   /= 10;
    }
    return ret;
}

/* 计算FreeDB disc ID */
uint32_t toc_calc_discid(const CdromToc *toc) {
    uint32_t checksum = 0;
    
    for (int i = toc->first_track; i <= toc->last_track; i++) {
        checksum += cddb_sum(toc->tracks[i].offset / 75);
    }
    
    uint32_t total_secs =
        toc->leadout / 75 - toc->tracks[toc->first_track].offset / 75;
    
    uint32_t track_count =
        (uint32_t)(toc->last_track - toc->first_track + 1);
    
    return ((checksum % 0xFF) << 24)
         | (total_secs        << 8)
         |  track_count;
}

/* 格式化为8位hex字符串 */
void toc_discid_str(const CdromToc *toc, char out[9]) {
    uint32_t id = toc_calc_discid(toc);
    snprintf(out, 9, "%08x", id);
}
