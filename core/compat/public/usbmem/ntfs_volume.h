// SPDX-License-Identifier: GPL-2.0-or-later
// C++ から NTFS を使うラッパ (LinuxCompat + fs/ntfs を内部で使うため GPL-2.0-or-later 扱い)。
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "usbmem/block_device.h"
#include "usbmem/fs_volume.h"

namespace usbmem {

class NtfsVolume final : public FsVolume {
 public:
  // 読み取り専用でマウント。失敗時 nullptr と errno を *err に返す(負の errno ではなく正の値)。
  static std::unique_ptr<NtfsVolume> mount(BlockDevicePtr dev, int* err = nullptr);
  // 読み書きでマウント (実験的)。ビルドで USBMEM_NTFS_WRITE=ON が必要。無効なら err=EROFS。
  static std::unique_ptr<NtfsVolume> mount_rw(BlockDevicePtr dev, int* err = nullptr);
  const char* fs_name() const override { return "ntfs"; }
  int sync() override;
  // 名前空間の変更 (実験的)。0 / 負の errno。sync() で永続化。
  int create(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino) override;
  int mkdir(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino) override;
  int unlink(uint64_t dir, const std::string& name) override;
  int rmdir(uint64_t dir, const std::string& name) override;
  int rename(uint64_t old_dir, const std::string& old_name, uint64_t new_dir, const std::string& new_name) override;
  int64_t write(uint64_t ino, uint64_t off, const void* buf, size_t len) override;   // 実験的: 拡張可。sync() で永続化
  bool writable() const override;                        // rw 要求でも、異常 (ダーティ等) で読み取り専用に落ちていれば false
  int truncate(uint64_t ino, uint64_t size) override;   // 実験的: 0 / 負の errno
  int64_t write_inplace(uint64_t ino, uint64_t off, const void* buf, size_t len) override;
  ~NtfsVolume() override;
  uint64_t root() const override { return 5; }
  int lookup(uint64_t dir, const std::string& name, uint64_t* ino) override;
  int stat(uint64_t ino, NtfsStat* st) override;
  int readdir(uint64_t dir, std::vector<NtfsDirEntry>* out) override;
  int64_t read(uint64_t ino, uint64_t off, void* buf, size_t len) override;

 private:
  friend std::unique_ptr<NtfsVolume> mount_impl_(BlockDevicePtr, int, int*);
  NtfsVolume() = default;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace usbmem
