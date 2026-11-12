// SPDX-License-Identifier: GPL-2.0-or-later
// C++ から ext4 を使うラッパ (LinuxCompat + fs/ext4。GPL-2.0-or-later 扱い)。読み取り専用・journal は再生しない。
#pragma once
#include "usbmem/fs_volume.h"

namespace usbmem {

struct Ext4MountOptions {
  // false (既定): ジャーナルの未再生 (needs_recovery) があるボリュームは EUCLEAN で拒否する。
  // true: ジャーナルを無視 (noload) してマウントする。未再生の更新は反映されないので古い/不整合なデータが見えうる。アプリはユーザーの明示的な確認の後だけ true にする (読み取り専用のみ)。
  bool allow_noload = false;
};

class Ext4Volume final : public FsVolume {
 public:
  // 読み取り専用でマウント。失敗時 nullptr と errno (正の値) を *err に返す。EUCLEAN = 要ジャーナル回復 / 破損、ENOTSUP 系 = 未対応機能。
  static std::unique_ptr<Ext4Volume> mount(BlockDevicePtr dev, int* err = nullptr, const Ext4MountOptions& opts = {});
  ~Ext4Volume() override;
  const char* fs_name() const override { return "ext4"; }
  uint64_t root() const override { return 2; }   // USBMEM_EXT4_ROOT_INO
  int lookup(uint64_t dir, const std::string& name, uint64_t* ino) override;
  int stat(uint64_t ino, FsStat* st) override;
  int readdir(uint64_t dir, std::vector<FsDirEntry>* out) override;
  int64_t read(uint64_t ino, uint64_t off, void* buf, size_t len) override;
  int readlink(uint64_t ino, std::string* target) override;   // シンボリックリンクのリンク先 (LinkResolver が使う)

 private:
  Ext4Volume() = default;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace usbmem
