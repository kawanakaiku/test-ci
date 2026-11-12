// SPDX-License-Identifier: GPL-2.0-or-later
// XFS の読み取り専用ボリューム (ディスク形式を独立に実装したリーダ。ログの再生・書き込みの経路は持たない)。docs/xfs-port.md
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
  ~XfsVolume() override;
  const char* fs_name() const override { return "xfs"; }
  uint64_t root() const override;
  int lookup(uint64_t dir, const std::string& name, uint64_t* ino) override;
  int stat(uint64_t ino, FsStat* st) override;
  int readdir(uint64_t dir, std::vector<FsDirEntry>* out) override;
  int64_t read(uint64_t ino, uint64_t off, void* buf, size_t len) override;
  int readlink(uint64_t ino, std::string* target) override;
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
