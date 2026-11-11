// SPDX-License-Identifier: GPL-2.0-or-later
// C++ から exFAT を使うラッパ (LinuxCompat + fs/exfat。GPL-2.0-or-later 扱い)。現状は読み取り専用。
#pragma once
#include "usbmem/fs_volume.h"

namespace usbmem {

class ExfatVolume final : public FsVolume {
 public:
  // 読み取り専用でマウント。失敗時 nullptr と errno (正の値) を *err に返す。
  static std::unique_ptr<ExfatVolume> mount(BlockDevicePtr dev, int* err = nullptr);
  ~ExfatVolume() override;
  const char* fs_name() const override { return "exfat"; }
  uint64_t root() const override { return 1; }   // USBMEM_EXFAT_ROOT_INO
  int lookup(uint64_t dir, const std::string& name, uint64_t* ino) override;
  int stat(uint64_t ino, FsStat* st) override;
  int readdir(uint64_t dir, std::vector<FsDirEntry>* out) override;
  int64_t read(uint64_t ino, uint64_t off, void* buf, size_t len) override;

 private:
  ExfatVolume() = default;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace usbmem
