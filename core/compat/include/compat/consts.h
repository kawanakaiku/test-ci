/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Linux の定数 (値は Linux uapi / include/linux に合わせる)。 */
#ifndef USBMEM_COMPAT_CONSTS_H
#define USBMEM_COMPAT_CONSTS_H
#include <compat/fs.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <dirent.h>

#define BITS_PER_BYTE 8
#define SECTOR_SHIFT 9
#define SECTOR_SIZE (1 << SECTOR_SHIFT)
#define U8_MAX 255
#define U16_MAX 65535
#define EIOCBQUEUED 529
#define ERESTARTSYS 512
#define THIS_MODULE ((void *)0)
#define READ 0
#define WRITE 1
#define CAP_SYS_ADMIN 21
#define NLS_MAX_CHARSET_SIZE 6
#define PAGE_KERNEL 0
#define VM_MAP 0x4
#define VM_FAULT_SIGBUS 0x2
#define UTF16_LITTLE_ENDIAN 0
#define GLOBAL_ROOT_UID ((kuid_t){0})
#define GLOBAL_ROOT_GID ((kgid_t){0})
#define INVALID_UID ((kuid_t){(uid_t)-1})
#define INVALID_GID ((kgid_t){(gid_t)-1})
#define FSLABEL_MAX 256

/* mode: S_IF* は <sys/stat.h> が提供。DT_* は <dirent.h>。 */
#define S_IMMUTABLE (1 << 3)
#define S_APPEND (1 << 2)
#define S_NOATIME (1 << 7)
#define S_NOSEC (1 << 14)

/* super_block / mount */
#define SB_RDONLY 1
#define SB_SILENT 32768
#define SB_POSIXACL (1 << 16)
#define SB_ACTIVE (1 << 30)
#define SB_FORCE 2
#define FS_REQUIRES_DEV 1
#define FS_ALLOW_IDMAP 32
#define FS_CONTEXT_FOR_RECONFIGURE 2

/* file / open */
#define FMODE_READ 1u
#define FMODE_WRITE 2u
#define FMODE_NOWAIT (1u << 5)
#define FMODE_CAN_ODIRECT (1u << 22)
#define IOCB_DIRECT (1 << 17)
#define IOCB_NOWAIT (1 << 7)
#define MAY_EXEC 1
#define MAY_WRITE 2
#define MAY_READ 4
#define MAY_OPEN 32
#define LOOKUP_RCU 1
#define RENAME_EXCHANGE (1 << 1)
#define RENAME_WHITEOUT (1 << 2)
#define SEEK_DATA 3
#define SEEK_HOLE 4

/* inode state */
#define I_NEW (1 << 3)
#define I_SYNC (1 << 4)
#define I_FREEING (1 << 5)
#define I_WILL_FREE (1 << 6)
#define I_CREATING (1 << 15)
#define I_DIRTY_SYNC (1 << 0)
#define I_DIRTY_DATASYNC (1 << 1)
#define I_DIRTY_PAGES (1 << 2)
#define I_DIRTY_ALL (I_DIRTY_SYNC | I_DIRTY_DATASYNC | I_DIRTY_PAGES)
#define I_MUTEX_PARENT 1

/* fallocate / ioctl / fs flags */
#define FALLOC_FL_KEEP_SIZE 0x01
#define FALLOC_FL_PUNCH_HOLE 0x02
#define FALLOC_FL_COLLAPSE_RANGE 0x08
#define FALLOC_FL_INSERT_RANGE 0x20
#define FALLOC_FL_ALLOCATE_RANGE 0x00
#define FALLOC_FL_MODE_MASK 0x3f
#define FITRIM 0xC0185879
#define FS_IOC_GETFSLABEL 0x81009431
#define FS_IOC_SETFSLABEL 0x41009432
#define FS_IOC_SHUTDOWN 0x8004587D
#define FS_SHUTDOWN_FLAGS_DEFAULT 0
#define FS_SHUTDOWN_FLAGS_LOGFLUSH 1
#define FS_SHUTDOWN_FLAGS_NOLOGFLUSH 2
#define FS_COMPR_FL 0x4
#define FS_IMMUTABLE_FL 0x10
#define FS_APPEND_FL 0x20
#define FS_NODUMP_FL 0x40
#define FS_NOATIME_FL 0x80
#define FS_ENCRYPT_FL 0x800
#define FS_CASEFOLD_FL 0x40000000
#define XATTR_CREATE 1
#define XATTR_REPLACE 2

/* statx */
#define STATX_BTIME 0x800u
#define STATX_DIOALIGN 0x2000u
#define STATX_ATTR_COMPRESSED 0x4
#define STATX_ATTR_IMMUTABLE 0x10
#define STATX_ATTR_APPEND 0x20
#define STATX_ATTR_NODUMP 0x40
#define STATX_ATTR_ENCRYPTED 0x800

/* mm / slab / page cache */
#define SLAB_RECLAIM_ACCOUNT 0x00020000
#define SLAB_HWCACHE_ALIGN 0x00002000
#define SLAB_ACCOUNT 0x04000000
#define WQ_PERCPU 0
#define FGP_LOCK 1
#define FGP_CREAT 2
#define FGP_NOFS 0x10
#define FGP_NOWAIT 0x20
#define FGP_WRITEBEGIN (FGP_LOCK | FGP_CREAT)
#define WB_SYNC_ALL 1

/* block layer */
#define REQ_OP_READ 0
#define REQ_OP_WRITE 1
#define REQ_OP_MASK 0xff
#define REQ_SYNC (1u << 11)
#define REQ_META (1u << 12)
#define BIOSET_NEED_BVECS 1
#define BIO_POOL_SIZE 2
#define BLKDEV_ZERO_NOUNMAP 1

/* iomap */
#define IOMAP_HOLE 0
#define IOMAP_DELALLOC 1
#define IOMAP_MAPPED 2
#define IOMAP_UNWRITTEN 3
#define IOMAP_INLINE 4
#define IOMAP_NULL_ADDR (-1ULL)
#define IOMAP_F_NEW (1u << 0)
#define IOMAP_F_MERGED (1u << 2)
#define IOMAP_F_STALE (1u << 6)
#define IOMAP_WRITE (1 << 0)
#define IOMAP_ZERO (1 << 1)
#define IOMAP_REPORT (1 << 2)
#define IOMAP_DIRECT (1 << 4)

#define RB_ROOT ((struct rb_root){ NULL })
#define VMA_MAYWRITE_BIT 5
#define VMA_SHARED_BIT 3
#endif
