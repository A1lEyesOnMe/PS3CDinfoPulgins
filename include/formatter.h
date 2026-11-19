#pragma once
#include "musicbrainz.h"

/*
 * 将CDInfo格式化为CDDB协议响应（向后兼容）
 * 
 * CDDB响应格式（关键部分）：
 * 210 OK
 * DTITLE=Artist / Album
 * TTITLE0=Track 1
 * TTITLE1=Track 2
 * ...
 * .
 * 
 * buf: 输出缓冲区
 * buf_size: 缓冲区大小
 * info: CD信息
 * 
 * 返回：实际写入的字节数，失败返回-1
 */
int format_cddb_response(char *buf, size_t buf_size, const CDInfo *info);

/*
 * 构建AMG二进制响应（XMB原生格式）
 * 
 * 这是PS3 XMB真正期望的格式，而非CDDB文本格式。
 * 使用从simple-cd-info借鉴的AMG二进制协议构建。
 * 
 * buf: 输出缓冲区
 * buf_size: 缓冲区大小
 * info: CD信息
 * scratch: 临时工作区（用于构建嵌套容器）
 * scratch_size: 临时工作区大小
 * 
 * 返回：实际写入的字节数，失败返回-1
 */
int format_amg_response(char *buf, size_t buf_size, const CDInfo *info,
                        unsigned char *scratch, size_t scratch_size);
