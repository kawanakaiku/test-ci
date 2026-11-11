#pragma once
#include <compat/kernel.h>
/* uid/gid の 16 bit / 32 bit 変換 (カーネルの highuid.h。overflowuid = 65534)。 */
#define low_16_bits(x) ((x) & 0xFFFF)
#define high_16_bits(x) (((x) & 0xFFFF0000) >> 16)
#define DEFAULT_OVERFLOWUID 65534
#define DEFAULT_OVERFLOWGID 65534
#define overflowuid DEFAULT_OVERFLOWUID
#define overflowgid DEFAULT_OVERFLOWGID
#define fs_overflowuid DEFAULT_OVERFLOWUID
#define fs_overflowgid DEFAULT_OVERFLOWGID
#define high2lowuid(uid) ((uid) & ~0xFFFF ? (uid16_t)overflowuid : (uid16_t)(uid))
#define high2lowgid(gid) ((gid) & ~0xFFFF ? (gid16_t)overflowgid : (gid16_t)(gid))
#define low2highuid(uid) ((uid) == (uid16_t)-1 ? (uid_t)-1 : (uid_t)(uid))
#define low2highgid(gid) ((gid) == (gid16_t)-1 ? (gid_t)-1 : (gid_t)(gid))
#define fs_high2lowuid(uid) ((uid) & ~0xFFFF ? (uid16_t)fs_overflowuid : (uid16_t)(uid))
#define fs_high2lowgid(gid) ((gid) & ~0xFFFF ? (gid16_t)fs_overflowgid : (gid16_t)(gid))
#define fs_low2highuid(uid) ((uid) == (uid16_t)-1 ? (uid_t)-1 : (uid_t)(uid))
#define fs_low2highgid(gid) ((gid) == (gid16_t)-1 ? (gid_t)-1 : (gid_t)(gid))
typedef unsigned short uid16_t;
typedef unsigned short gid16_t;
