// SPDX-License-Identifier: GPL-2.0-or-later
// XFS のボリューム (ディスク形式を独立に実装したリーダ + 書き込み)。既定は読み取り専用で、ログの再生は持たない。書き込みは USBMEM_NTFS_WRITE=ON のビルドの mount_rw だけ。docs/xfs-port.md / docs/xfs-write.md
#pragma once
#include "usbmem/fs_volume.h"

namespace usbmem {

struct XfsMountOptions {
  // false (既定): ログに未再生の更新があるボリュームは EUCLEAN(117) で拒否する。
  // true: ログを無視 (noload) してマウントする。未再生の更新は反映されないので、古い/不整合なデータが見えうる。
  //       アプリはユーザーの明示的な確認の後だけ true にする (読み取り専用のみ)。
  bool allow_noload = false;
};

class XfsVolume final : public FsVolume {
 public:
  // 読み取り専用でマウント。失敗時 nullptr と errno (正の値) を *err に返す。
  // EUCLEAN = 要ログ回復 / 破損 / 要 xfs_repair、EOPNOTSUPP = 未対応機能 (リアルタイム領域・外部ログ・未知の incompat 機能)、EINVAL = XFS ではない。
  static std::unique_ptr<XfsVolume> mount(BlockDevicePtr dev, int* err = nullptr, const XfsMountOptions& opts = {});
  // 読み書きでマウント (実験的。docs/xfs-write.md)。ビルドで USBMEM_NTFS_WRITE=ON が必要 (無効なら err=EROFS)。
  // 書き込み可の条件 (V5 / CRC、ログがクリーン、未知の機能なし、rmapbt・クォータ・リアルタイム・外部ログ・ストライプ・ディレクトリブロック > ブロック なし、など) を
  // 満たさなければ、読み取り専用でマウントに成功する (writable() == false、理由は read_only_reason())。ログが未再生なら読み取り専用と同じく EUCLEAN で失敗する。
  // 変更はメモリに溜め、sync() / デストラクタでログ (カーネルが再生できる形式) 経由でコミットされる。
  static std::unique_ptr<XfsVolume> mount_rw(BlockDevicePtr dev, int* err = nullptr);
  ~XfsVolume() override;
  const char* fs_name() const override { return "xfs"; }
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
  // ログが未再生のまま (allow_noload) 開いているか
  bool journal_ignored() const;
  // 診断用: "v5 crc=1 ftype=1 bigtime=1 ..." のような要約
  std::string describe_features() const;

 private:
  XfsVolume() = default;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace usbmem
