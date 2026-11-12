// SPDX-License-Identifier: GPL-2.0-or-later
// C++ から erofs を使うラッパ (LinuxCompat + fs/erofs。GPL-2.0-or-later 扱い)。読み取り専用。
#pragma once
#include "usbmem/fs_volume.h"

namespace usbmem {

class ErofsVolume final : public FsVolume {
 public:
  // 読み取り専用でマウント。失敗時 nullptr と errno (正の値) を *err に返す。
  static std::unique_ptr<ErofsVolume> mount(BlockDevicePtr dev, int* err = nullptr);
  ~ErofsVolume() override;
  const char* fs_name() const override { return "erofs"; }
  uint64_t root() const override;
  int lookup(uint64_t dir, const std::string& name, uint64_t* ino) override;
  int stat(uint64_t ino, FsStat* st) override;
  int readdir(uint64_t dir, std::vector<FsDirEntry>* out) override;
  int64_t read(uint64_t ino, uint64_t off, void* buf, size_t len) override;
  int readlink(uint64_t ino, std::string* target) override;

 private:
  ErofsVolume() = default;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace usbmem
