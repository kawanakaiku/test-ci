// SPDX-License-Identifier: GPL-2.0-or-later
// Btrfs の読み取り専用ボリューム (自前のリーダー。Linux の fs/btrfs は載せていない: docs/btrfs-port.md)。
// 展開 (zlib / lzo / zstd) だけ third_party/kernel-lib (無改変) を使うので、リンクすると GPL-2.0-or-later 扱い。
// 書き込みは常に拒否 (EROFS)。複数デバイス / RAID0・5・6・10 / 暗号化 / zoned は理由付きで拒否 (EOPNOTSUPP)。
#pragma once
#include <string>
#include <vector>

#include "usbmem/fs_volume.h"

namespace usbmem {

struct BtrfsMountOptions {
  // false (既定): tree-log が空でない (fsync した更新が本体に反映されていない) ボリュームは EUCLEAN(117) で拒否する。
  // true: tree-log を無視してマウントする。直近の fsync 済みの更新が見えないことがある。アプリはユーザーの明示的な確認の後だけ true にする (読み取り専用のみ)。
  bool allow_noload = false;
  // 0 = 既定のサブボリューム (mount の既定)。0 以外ならそのサブボリューム ID を root にする。
  uint64_t subvol_id = 0;
  // データのチェックサム検証。既定で有効 (不一致は EIO。DUP などの別コピーがあればそちらを使う)。
  bool verify_data = true;
};

struct BtrfsSubvolume {
  uint64_t id = 0, parent_id = 0, dirid = 0;   // dirid: 親サブボリューム内の、このサブボリュームを置いているディレクトリの inode
  std::string name;                            // 親ディレクトリ内での名前 (トップレベル (ID 5) は空)
  bool read_only = false;
};

class BtrfsVolume final : public FsVolume {
 public:
  // 読み取り専用でマウント。失敗時 nullptr と errno (正の値) を *err に返す。EUCLEAN(117) = tree-log が未再生、EIO(5) = 破損、EOPNOTSUPP(95) = 未対応。
  static std::unique_ptr<BtrfsVolume> mount(BlockDevicePtr dev, int* err = nullptr, const BtrfsMountOptions& opts = {});
  ~BtrfsVolume() override;
  const char* fs_name() const override { return "btrfs"; }
  uint64_t root() const override;
  int lookup(uint64_t dir, const std::string& name, uint64_t* ino) override;
  int stat(uint64_t ino, FsStat* st) override;
  int readdir(uint64_t dir, std::vector<FsDirEntry>* out) override;
  int64_t read(uint64_t ino, uint64_t off, void* buf, size_t len) override;
  int readlink(uint64_t ino, std::string* target) override;

  // 全サブボリューム (スナップショットを含む) の一覧。ルート (ID 5) も含む。
  int list_subvolumes(std::vector<BtrfsSubvolume>* out);
  uint64_t mounted_subvolume() const;
  bool log_ignored() const;   // allow_noload で、実際に未再生の tree-log を無視して開いた
  std::string label() const;

 private:
  BtrfsVolume() = default;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace usbmem
