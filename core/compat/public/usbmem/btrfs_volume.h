// SPDX-License-Identifier: GPL-2.0-or-later
// Btrfs の読み取り専用ボリューム (自前のリーダー。Linux の fs/btrfs は載せていない: docs/btrfs-port.md)。
// 展開 (zlib / lzo / zstd) だけ third_party/kernel-lib (無改変) を使うので、リンクすると GPL-2.0-or-later 扱い。
// 既定は読み取り専用 (書き込みは EROFS)。mount_rw (USBMEM_NTFS_WRITE=ON のビルド) は、条件を満たす single / DUP のボリュームだけ書き込み可にする (docs/btrfs-write.md)。
// 複数デバイス / RAID0・5・6・10 / 暗号化 / zoned は理由付きで拒否 (EOPNOTSUPP)。
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
  // 読み書きでマウント (実験的。docs/btrfs-write.md)。ビルドで USBMEM_NTFS_WRITE=ON が必要 (無効なら err=EROFS)。
  // 書き込み可の条件 (single / DUP のみ、free space tree・no-holes・skinny あり、サブボリューム / クォータ / 未再生ログなし、
  // スーパーブロックのミラーが一致、など) を満たさなければ、読み取り専用でマウントに成功する (writable() == false、理由は read_only_reason())。
  // クリーンなボリュームは最初の書き込みまでメディアに何も書かない。変更は sync() / デストラクタでコミットされる。
  static std::unique_ptr<BtrfsVolume> mount_rw(BlockDevicePtr dev, int* err = nullptr);
  ~BtrfsVolume() override;
  const char* fs_name() const override { return "btrfs"; }
  uint64_t root() const override;
  int lookup(uint64_t dir, const std::string& name, uint64_t* ino) override;
  int stat(uint64_t ino, FsStat* st) override;
  int readdir(uint64_t dir, std::vector<FsDirEntry>* out) override;
  int64_t read(uint64_t ino, uint64_t off, void* buf, size_t len) override;
  int readlink(uint64_t ino, std::string* target) override;
  bool writable() const override;
  std::string read_only_reason() const;   // mount_rw で書き込み可にしなかった / 途中で書き込みをやめた理由 (書き込み可なら空)
  int sync() override;
  int create(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino) override;
  int mkdir(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino) override;
  int unlink(uint64_t dir, const std::string& name) override;
  int rmdir(uint64_t dir, const std::string& name) override;
  int rename(uint64_t od, const std::string& on, uint64_t nd, const std::string& nn) override;
  int64_t write(uint64_t ino, uint64_t off, const void* buf, size_t len) override;
  int truncate(uint64_t ino, uint64_t size) override;
  int64_t write_inplace(uint64_t ino, uint64_t off, const void* buf, size_t len) override;
  int symlink(uint64_t dir, const std::string& name, const std::string& target, uint64_t* ino) override;
  int link(uint64_t dir, const std::string& name, uint64_t target_ino) override;

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
