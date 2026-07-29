#pragma once
#include "musicbrainz.h"

/*
 * 将CDInfo格式化为CDDB协议响应
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
