/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef WIMFUSE_IO_H
#define WIMFUSE_IO_H
/* wimlib に WIM ファイルとして渡すパス (wimfuse_io.c が open / realpath を横取りする。実在しない) */
#define WIMFUSE_SRC_PATH "usbmem-rpc://wim"
#endif
