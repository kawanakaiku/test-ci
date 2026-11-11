// SPDX-License-Identifier: GPL-2.0-or-later
// C++ から ext2 を使うラッパ (LinuxCompat + fs/ext2。GPL-2.0-or-later 扱い)。読み取りは常に可。書き込みはビルド (USBMEM_NTFS_WRITE=ON) と mount_rw の両方で有効にしたときだけ。
#pragma once
#include "usbmem/fs_volume.h"

namespace usbmem {

class Ext2Volume final : public FsVolume {
 public:
  // 読み取り専用でマウント。失敗時 nullptr と errno (正の値) を *err に返す。
  static std::unique_ptr<Ext2Volume> mount(BlockDevicePtr dev, int* err = nullptr);
  // 読み書きでマウント (実験的)。ビルドで USBMEM_NTFS_WRITE=ON が必要 (無効なら err=EROFS)。前回きれいに unmount されていない・未知の機能がある場合は
  // 読み取り専用でマウントに成功する (writable() == false)。マウント自体はメディアに何も書かない: 最初の書き込みの直前にダーティ印を確定する。
  static std::unique_ptr<Ext2Volume> mount_rw(BlockDevicePtr dev, int* err = nullptr);
  ~Ext2Volume() override;
  const char* fs_name() const override { return "ext2"; }
  uint64_t root() const override { return 2; }   // USBMEM_EXT2_ROOT_INO
  int lookup(uint64_t dir, const std::string& name, uint64_t* ino) override;
  int stat(uint64_t ino, FsStat* st) override;
  int readdir(uint64_t dir, std::vector<FsDirEntry>* out) override;
  int64_t read(uint64_t ino, uint64_t off, void* buf, size_t len) override;
  bool writable() const override;
  int sync() override;
  int create(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino) override;
  int mkdir(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino) override;
  int unlink(uint64_t dir, const std::string& name) override;
  int rmdir(uint64_t dir, const std::string& name) override;
  int rename(uint64_t od, const std::string& on, uint64_t nd, const std::string& nn) override;
  int64_t write(uint64_t ino, uint64_t off, const void* buf, size_t len) override;
  int truncate(uint64_t ino, uint64_t size) override;
  int64_t write_inplace(uint64_t ino, uint64_t off, const void* buf, size_t len) override;
  int readlink(uint64_t ino, std::string* target) override;
  int symlink(uint64_t dir, const std::string& name, const std::string& target, uint64_t* ino) override;
  int link(uint64_t dir, const std::string& name, uint64_t target_ino) override;

 private:
  Ext2Volume() = default;
  static std::unique_ptr<Ext2Volume> mount_impl(BlockDevicePtr dev, int ro, int* err);
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace usbmem
