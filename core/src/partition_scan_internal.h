// SPDX-License-Identifier: Apache-2.0
// partition_scan*.cpp / partition_recover.cpp の内部共有部品 (公開しない)。
#pragma once
#include <cstring>
#include <string>
#include <vector>

#include "usbmem/partition_scan.h"

namespace usbmem {
namespace ps {

inline uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
inline uint32_t le32(const uint8_t* p) {
  return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) |
         (static_cast<uint32_t>(p[3]) << 24);
}
inline uint64_t le64(const uint8_t* p) { return le32(p) | (static_cast<uint64_t>(le32(p + 4)) << 32); }
inline uint16_t be16(const uint8_t* p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
inline uint32_t be32(const uint8_t* p) {
  return (static_cast<uint32_t>(p[0]) << 24) | (static_cast<uint32_t>(p[1]) << 16) | (static_cast<uint32_t>(p[2]) << 8) | p[3];
}
inline uint64_t be64(const uint8_t* p) { return (static_cast<uint64_t>(be32(p)) << 32) | be32(p + 4); }
inline bool is_pow2(uint64_t v) { return v && !(v & (v - 1)); }
inline bool all_zero(const uint8_t* p, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (p[i]) return false;
  return true;
}
inline bool mul_ok(uint64_t a, uint64_t b, uint64_t* r) { return !__builtin_mul_overflow(a, b, r); }
inline bool add_ok(uint64_t a, uint64_t b, uint64_t* r) { return !__builtin_add_overflow(a, b, r); }

// デバイスの絶対バイト位置での読み取り。範囲外は false (エラーにしない)。I/O エラーは数える。Disconnected / Closed / Cancelled は fatal に残す。
class Reader {
 public:
  Reader(BlockDevice& d, const CancelToken* c) : dev_(d), cancel_(c), disk_(d.size()) {}
  bool read(uint64_t off, void* buf, size_t n);
  uint64_t disk() const { return disk_; }
  bool stopped() const { return !fatal.ok() || (cancel_ && cancel_->cancelled()); }
  Status fatal{};
  uint64_t io_errors = 0, bytes_read = 0;

 private:
  BlockDevice& dev_;
  const CancelToken* cancel_;
  uint64_t disk_;
};

// 検証済みの FS の発見
struct Hit {
  FsType fs = FsType::Unknown;
  std::string variant;
  uint64_t start = 0;  // バイト
  uint64_t size = 0;   // バイト (size_known == false なら 0 か下限)
  bool size_known = false, size_min = false;
  int score = 0;
  bool primary_ok = false, backup_ok = false;
  std::vector<std::string> notes;
};

enum class Kind : uint8_t { Fat, Exfat, Ntfs, Ext, Xfs, Btrfs, F2fs, Squashfs, Erofs, Iso, Luks, Bitlocker };

// 512 バイトのチャンク (絶対位置 addr) に見つかったシグネチャから「パーティションの開始位置の仮説」を作る。
struct Hyp {
  uint64_t start;
  Kind kind;
  uint64_t aux;  // Ext: 見つけたスーパーブロックの開始からの相対位置 (バックアップの場合)。他は 0
};
void match_anchors(const uint8_t* chunk, uint64_t addr, uint32_t ss, std::vector<Hyp>* out);

// 仮説の検証 (開始位置 start に kind の FS があるか)。成功なら true を返し hit を埋める。
bool verify(Reader& rd, const Hyp& h, uint32_t ss, Hit* hit);

// 同じ位置・同じ系統の FS を 1 つにまとめるための系統
int family_of(FsType t);

// ---- オンディスク構造の解析 (パーティション探索とブートセクタの検査が共用する。書き込みはしない)
struct Fat {
  uint32_t bps = 0, spc = 0, rsvd = 0, nfats = 0, root_ent = 0, media = 0, root_clus = 0, fsinfo = 0, bk = 0;
  uint64_t tot = 0, fatsz = 0;
  bool fat32 = false;
  FsType type = FsType::Unknown;
};
bool parse_fat(const uint8_t* sector512, Fat* f);  // FAT12/16/32 のブートセクタ (BPB の整合まで検査する)
struct Exfat { uint32_t bps = 0; uint64_t vol_len = 0; };
bool parse_exfat(const uint8_t* sector512, Exfat* x);
// exFAT のブート領域 (12 セクタ) のチェックサム。volume flags (106, 107) と percent in use (112) を除く
uint32_t exfat_boot_checksum(const uint8_t* region, uint32_t bps);
struct Ntfs { uint32_t bps = 0, spc = 0; uint64_t total = 0, mft = 0, mirr = 0; };
bool parse_ntfs(const uint8_t* sector512, Ntfs* n);
struct ExtSb {
  uint32_t bs = 0, bpg = 0, ipg = 0, fdb = 0, group = 0, compat = 0, incompat = 0, ro = 0, state = 0;
  uint64_t blocks = 0, groups = 0;
  bool csum_ok = false, csum_present = false;
  uint8_t uuid[16];
};
bool parse_ext(const uint8_t* sb1024, ExtSb* e);
FsType ext_type(const ExtSb& e);
uint64_t ext_sb_pos(uint64_t group, const ExtSb& e);  // パーティション先頭からのバイト位置

// 回復用の補助 (partition_recover.cpp / bootfix も使う)
uint32_t crc32c_ext_sb(const uint8_t* sb1024);  // ext の s_checksum 用 (先頭 1020 バイト)

}  // namespace ps
}  // namespace usbmem
