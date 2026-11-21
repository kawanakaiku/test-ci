// SPDX-License-Identifier: Apache-2.0
// kfsd のプロトコル (本体 <-> ファイルシステム・ドライバのプロセス)。仕様の全文は docs/kfsd.md。
//
// kfsd は、カーネル由来 (GPL) のファイルシステム・ドライバを載せた別の実行ファイル。本体 (Apache-2.0) は、この仕様のメッセージを
// 1 本の SOCK_STREAM のソケット (socketpair。Android では ParcelFileDescriptor 経由) で交換するだけで、ドライバのコードを一切リンクしない。
// このヘッダは「プロトコルの事実」だけを定義する (Apache-2.0)。実装は kfsd_client.h (本体側) / kfsd_server.h (kfsd 側の共通部分)。
//
// 通信の形 (同期・1 要求ずつ):
//   本体 -> kfsd : 要求 (Hdr + payload)。kfsd は 1 つの要求を処理している間に、ブロックデバイスの読み書きを本体へ頼める (Dev* の callback)。
//   kfsd -> 本体 : (0 回以上の) Dev* の callback の後、必ず 1 つの Reply で要求を終える。本体は callback へ Reply で答える。
//   つまり、ソケット上で同時に進む要求は常に 1 つ。callback は入れ子になるだけで並行しない。
// バイト順: リトルエンディアン (x86 / ARM64 / ARM の Android とホスト。ビッグエンディアンの環境は対象外で、static_assert で止める)。
#pragma once
#include <cstddef>
#include <cstdint>

#if !defined(__BYTE_ORDER__) || __BYTE_ORDER__ != __ORDER_LITTLE_ENDIAN__
#error "kfsd protocol is little-endian only"
#endif

namespace usbmem {
namespace kfsd {

constexpr uint32_t kMagic = 0x3153464Bu;       // "KFS1" (リトルエンディアンで 4B 46 53 31)
constexpr uint32_t kVersion = 1;
constexpr uint32_t kMaxPayload = 8u << 20;      // 1 メッセージの payload の上限。これを超える Hdr.len は受け取らず、接続を切る
constexpr uint32_t kMaxIo = 4u << 20;           // read / write / Dev* で 1 メッセージに載せるデータの上限 (呼び出し側が分割する)
constexpr uint32_t kMaxName = 4096;             // 名前 (パスの 1 要素) の長さの上限
constexpr uint32_t kReaddirPageBytes = 256u << 10;   // readdir 1 ページの目安 (これを超えたら次のページ)

// 要求 (本体 -> kfsd)
enum Op : uint32_t {
  kHello = 1,
  kMount,
  kLookup,
  kStat,
  kReaddir,
  kRead,
  kReadlink,
  kSync,
  kCreate,
  kMkdir,
  kUnlink,
  kRmdir,
  kRename,
  kWrite,
  kTruncate,
  kWriteInplace,
  kSymlink,
  kLink,
  kInfo,
  kStats,
  kClose,            // 正常なアンマウント (FsVolume の破棄) をして Reply を返し、kfsd は終了する
  // callback (kfsd -> 本体)。本体のブロックデバイスを読み書きする
  kDevRead = 0x101,
  kDevWrite = 0x102,
  kDevFlush = 0x103,
  // 応答 (どちら向きにも使う: 要求への答え / callback への答え)
  kReply = 0x200,
};

// 全メッセージの先頭 (40 バイト)。a / b / c の意味は op ごと (下の表と docs/kfsd.md)。Reply では a = rc (0 以上 = 成功、負 = -errno)。
struct Hdr {
  uint32_t op;
  uint32_t len;      // 後ろに続く payload のバイト数 (<= kMaxPayload)
  uint64_t seq;      // 要求の通し番号 (本体が付ける)。Reply は要求の seq をそのまま返す。callback の seq は kfsd が付ける
  uint64_t a, b, c;
};
static_assert(sizeof(Hdr) == 40, "Hdr is 40 bytes");

// ---- 各 op の引数 (a, b, c と payload) と Reply ----
//  kHello      payload = HelloMsg。Reply payload = HelloMsg (kfsd 側の version / name)。最初の 1 回だけ。
//  kMount      payload = MountReq。Reply a = rc、成功なら payload = MountRep。
//  kLookup     a = dir、payload = 名前。Reply b = ino。
//  kStat       a = ino。Reply payload = WireStat。
//  kReaddir    a = dir、b = cookie (最初は 0)、c = ReaddirFlags。Reply payload = WireDirent の列、b = 次の cookie、c = 1 なら続きがある。
//  kRead       a = ino、b = offset、c = len (<= kMaxIo)。Reply a = 読めたバイト数 (0 = EOF)、payload = そのデータ。
//  kReadlink   a = ino。Reply payload = リンク先。
//  kSync       Reply a = rc。
//  kCreate / kMkdir     a = dir、b = mode、payload = 名前。Reply b = ino。
//  kUnlink / kRmdir     a = dir、payload = 名前。
//  kRename     a = old_dir、b = new_dir、payload = u32 old_len, u32 new_len, old 名, new 名。
//  kWrite / kWriteInplace   a = ino、b = offset、payload = データ (<= kMaxIo)。Reply a = 書けたバイト数。
//  kTruncate   a = ino、b = size。
//  kSymlink    a = dir、payload = u32 name_len, 名前, リンク先。Reply b = ino。
//  kLink       a = dir、b = 対象の ino、payload = 名前。
//  kInfo       Reply payload = InfoRep (writable / state は変わりうるので毎回問い合わせる)。
//  kStats      Reply payload = StatsRep。
//  kClose      Reply a = rc (アンマウントの結果)。
//  kDevRead    a = offset、b = len (<= kMaxIo)。Reply a = 0 / -errno、成功なら payload = len バイト (短い読みは無い)。
//  kDevWrite   a = offset、payload = データ (<= kMaxIo)。Reply a = 0 / -errno。
//  kDevFlush   Reply a = 0 / -errno。

enum ReaddirFlags : uint64_t {
  kReaddirWithStat = 1,   // 各エントリに WireStat を付ける (lookup + stat の往復を省く)
};

enum MountFlags : uint32_t {
  kMountRw = 1,           // 書き込み可を要求 (ドライバが書けないと判断したら読み取り専用に落ちる。InfoRep.writable で分かる)
  kMountAllowNoload = 2,  // ジャーナルなどの未再生を無視して読み取り専用で開くことを、利用者が確認した
  kMountAllowDirty = 4,   // NTFS: ダーティフラグだけのボリュームを書き込み可で開くことを許す (既定の挙動。立てないと保留して読み取り専用で返す)
};

struct HelloMsg {
  uint32_t magic, version;
  uint32_t max_payload, reserved;
  char name[32];     // 相手の名前 ("usbmem-kfsd 1" など。ログ用)
};
static_assert(sizeof(HelloMsg) == 48, "HelloMsg");

struct MountReq {
  uint32_t flags;          // MountFlags
  uint32_t block_size;     // デバイスの論理ブロックサイズ
  uint64_t dev_size;       // デバイスのバイト数
  int32_t tz_minutes_west; // FAT の更新時刻の変換用 (UTC より何分西か。既定 0)
  uint32_t dev_read_only;  // 1 = 下のデバイスが読み取り専用 (書き込み可を要求しても読み取り専用でマウントする)
  uint32_t dev_readahead_kb;  // kfsd 側の連続読みの先読みの上限 (KiB。0 = 先読みなし)。連続して読んでいるときだけ 64 KiB から倍々に増やし、この値で止める
  uint32_t dev_write_coalesce_kb;  // kfsd 側で、連続する小さなデバイス書き込みをまとめて送る上限 (KiB。0 = まとめない)。要求の終わり (Reply の前) と、読み・flush・飛び飛びの書き込みの前に必ず本体へ書き切る
  uint64_t reserved1;
};
static_assert(sizeof(MountReq) == 40, "MountReq");

struct InfoRep {
  uint32_t writable;       // 0 / 1
  uint32_t state;          // USBMEM_VOL_* (vol_state.h)
  uint64_t root;
  char fs_name[24];        // FsVolume::fs_name() ("ntfs" / "ext4" ...。NUL 終端)
};
static_assert(sizeof(InfoRep) == 40, "InfoRep");

struct MountRep {
  InfoRep info;
  char fs_type[24];        // probe_filesystem の結果 ("ext4" / "fat32" / "iso9660" ...。NUL 終端)
};
static_assert(sizeof(MountRep) == 64, "MountRep");

struct WireStat {
  uint64_t ino, size;
  uint32_t mode, nlink;
  int64_t mtime;
};
static_assert(sizeof(WireStat) == 32, "WireStat");

// readdir の 1 エントリ: WireDirentHead + name (name_len バイト。NUL なし) + (dtype の kDirentHasStat が立っていれば WireStat)。次のエントリは直後 (詰める)。
// WireStat が付くのは kReaddirWithStat を指定したときだけ。そのエントリの stat がドライバから取れなかったら付かない (kDirentHasStat なし)。
constexpr uint32_t kDirentHasStat = 0x80000000u;
struct WireDirentHead {
  uint64_t ino;
  uint32_t dtype;     // 下位 8 bit = DT_* (FsDirEntry::dtype)、最上位 bit = kDirentHasStat
  uint32_t name_len;
};
static_assert(sizeof(WireDirentHead) == 16, "WireDirentHead");

struct StatsRep {
  uint64_t dev_read_calls, dev_read_bytes, dev_write_calls, dev_write_bytes, requests;
};
static_assert(sizeof(StatsRep) == 40, "StatsRep");

// 名前として受け付けない文字列 (空・'/' や NUL を含む・長すぎる)。ドライバから返ってきた名前も、本体はこれで検査する。
inline bool valid_name(const char* p, size_t n) {
  if (n == 0 || n > kMaxName) return false;
  for (size_t i = 0; i < n; i++)
    if (p[i] == '/' || p[i] == '\0') return false;
  return true;
}

}  // namespace kfsd
}  // namespace usbmem
