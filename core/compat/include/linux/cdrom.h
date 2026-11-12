/* SPDX-License-Identifier: GPL-2.0-or-later */
/* <linux/cdrom.h> 互換 (fs/isofs, fs/udf)。USB の記憶装置には CD-ROM のマルチセッション/TOC の ioctl が無いので、
 * `disk_to_cdi()` は常に NULL (上流は NULL のとき最初のセッション 0 を使う)。 */
#pragma once
#include <compat/kernel.h>

struct cdrom_device_info;
struct gendisk;
#define disk_to_cdi(disk) ((struct cdrom_device_info *)NULL)

struct cdrom_tocentry { u8 cdte_track; u8 cdte_adr:4; u8 cdte_ctrl:4; u8 cdte_format; union { struct { u8 minute, second, frame; } msf; int lba; } cdte_addr; u8 cdte_datamode; };
struct cdrom_multisession { union { struct { u8 minute, second, frame; } msf; int lba; } addr; u8 xa_flag; u8 addr_format; };
#define CDROM_LBA 0x01
#define CDROM_MSF 0x02
#define CDROM_DATA_TRACK 0x04
#define CDROMMULTISESSION 0x5310
#define CDROM_LAST_WRITTEN 0x5395
#define CDROM_GET_CAPABILITY 0x5331
#define CDROM_DRIVE_STATUS 0x5326
static inline int cdrom_read_tocentry(struct cdrom_device_info *cdi, struct cdrom_tocentry *te) { (void)cdi; (void)te; return -EINVAL; }
static inline int cdrom_multisession(struct cdrom_device_info *cdi, struct cdrom_multisession *ms) { (void)cdi; (void)ms; return -EINVAL; }
