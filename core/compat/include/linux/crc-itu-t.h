/* SPDX-License-Identifier: GPL-2.0-or-later */
/* CRC-ITU-T (多項式 0x1021, 非反転)。実体は src/rt_vfs_udf.c。udf のディスクリプタタグの CRC に使う。 */
#pragma once
#include <stdint.h>
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif
uint16_t crc_itu_t(uint16_t crc, const uint8_t *buffer, size_t len);
static inline uint16_t crc_itu_t_byte(uint16_t crc, const uint8_t data) { return crc_itu_t(crc, &data, 1); }
#ifdef __cplusplus
}
#endif
