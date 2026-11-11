// SPDX-License-Identifier: GPL-2.0-or-later
// C++ から NTFS を使うラッパ (LinuxCompat + fs/ntfs を内部で使うため GPL-2.0-or-later 扱い)。
#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "usbmem/block_device.h"

namespace usbmem {

struct NtfsStat { uint64_t ino = 0, size = 0; uint32_t mode = 0, nlink = 0; int64_t mtime = 0; bool is_dir() const { return (mode & 0170000) == 0040000; } };
struct NtfsDirEntry { std::string name; uint64_t ino; unsigned dtype; };

class NtfsVolume {
 public:
  // 読み取り専用でマウント。失敗時 nullptr と errno を *err に返す(負の errno ではなく正の値)。
  static std::unique_ptr<NtfsVolume> mount(BlockDevicePtr dev, int* err = nullptr);
  ~NtfsVolume();
  uint64_t root() const { return 5; }
  int lookup(uint64_t dir, const std::string& name, uint64_t* ino);
  int stat(uint64_t ino, NtfsStat* st);
  int readdir(uint64_t dir, std::vector<NtfsDirEntry>* out);
  int64_t read(uint64_t ino, uint64_t off, void* buf, size_t len);
  // "/a/b" 形式のパス解決 (区切りは '/')。
  int resolve(const std::string& path, uint64_t* ino);

 private:
  NtfsVolume() = default;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace usbmem
