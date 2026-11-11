// SPDX-License-Identifier: GPL-2.0-or-later
// ファイルシステム共通のボリューム・インタフェース (LinuxCompat 上のカーネル FS ドライバ用)。NTFS / exFAT などがこれを実装する。
// ino はボリューム内で有効な識別子 (NTFS: MFT 番号。exFAT: 実行時採番で unmount まで有効)。パスは "/a/b" 形式。
#pragma once
#include <memory>
#include <string>
#include <vector>

#include "usbmem/block_device.h"

namespace usbmem {

struct NtfsStat { uint64_t ino = 0, size = 0; uint32_t mode = 0, nlink = 0; int64_t mtime = 0; bool is_dir() const { return (mode & 0170000) == 0040000; } };
struct NtfsDirEntry { std::string name; uint64_t ino; unsigned dtype; };
using FsStat = NtfsStat;
using FsDirEntry = NtfsDirEntry;

class FsVolume {
 public:
  virtual ~FsVolume() = default;
  virtual const char* fs_name() const = 0;   // "ntfs" / "exfat" ...
  virtual uint64_t root() const = 0;
  virtual int lookup(uint64_t dir, const std::string& name, uint64_t* ino) = 0;
  virtual int stat(uint64_t ino, FsStat* st) = 0;
  virtual int readdir(uint64_t dir, std::vector<FsDirEntry>* out) = 0;
  virtual int64_t read(uint64_t ino, uint64_t off, void* buf, size_t len) = 0;
  // 書き込み系は既定で未対応 (読み取り専用の FS)。0 / 負の errno。
  virtual bool writable() const { return false; }
  virtual int sync() { return -30; /* EROFS */ }
  virtual int create(uint64_t, const std::string&, uint32_t, uint64_t*) { return -30; }
  virtual int mkdir(uint64_t, const std::string&, uint32_t, uint64_t*) { return -30; }
  virtual int unlink(uint64_t, const std::string&) { return -30; }
  virtual int rmdir(uint64_t, const std::string&) { return -30; }
  virtual int rename(uint64_t, const std::string&, uint64_t, const std::string&) { return -30; }
  virtual int64_t write(uint64_t, uint64_t, const void*, size_t) { return -30; }
  virtual int truncate(uint64_t, uint64_t) { return -30; }
  virtual int64_t write_inplace(uint64_t, uint64_t, const void*, size_t) { return -30; }
  // リンク (既定は未対応 = -EOPNOTSUPP)。readlink はリンク先の文字列、symlink / link は書き込み系 (ハードリンクはディレクトリ不可)。
  virtual int readlink(uint64_t, std::string*) { return -95; }
  virtual int symlink(uint64_t, const std::string&, const std::string&, uint64_t*) { return -95; }
  virtual int link(uint64_t, const std::string&, uint64_t) { return -95; }
  // "/a/b" 形式のパス解決 (区切りは '/')。
  int resolve(const std::string& path, uint64_t* ino);
};

}  // namespace usbmem
