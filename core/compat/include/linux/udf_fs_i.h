/* SPDX-License-Identifier: GPL-2.0 WITH Linux-syscall-note */
/* uapi/linux/udf_fs_i.h 相当 (fs/udf/file.c の ioctl 番号。ホスト入口は ioctl を使わない) */
#ifndef _UDF_FS_I_H
#define _UDF_FS_I_H 1
#define UDF_GETEASIZE   0x40046c40
#define UDF_GETEABLOCK  0x40086c41
#define UDF_GETVOLIDENT 0x40086c42
#define UDF_RELOCATE_BLOCKS 0xc0086c43
#endif
