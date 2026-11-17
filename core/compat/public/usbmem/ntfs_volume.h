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
  // ログの再生は実装しない。上流の書き込みマウントは $LogFile を空にして (= 未再生の更新を捨てて) 書き始めるので、先に読み取り専用でマウントして状態を調べ、
  // $LogFile に未再生のログがある / Windows が休止状態 (高速スタートアップ) のボリュームは、書き込みを要求されても読み取り専用のまま返す (writable() == false、state() で理由が分かる。docs/dirty-volumes.md)。
  // ダーティフラグだけ (ログは静止) のボリュームは、allow_dirty = true (既定) なら従来どおり書き込み可で開き、クリーンなアンマウントでフラグを下ろす (上流の方針)。
  // allow_dirty = false なら、書き込みマウントをせずに読み取り専用のまま返す (writable() == false、state() に USBMEM_VOL_DIRTY | USBMEM_VOL_CONFIRM_WRITE)。
  // UI が利用者に確認するための保留で、イメージは 1 バイトも変わらない。確認が取れたら allow_dirty = true で開き直す (docs/dirty-volumes.md)。
  static std::unique_ptr<NtfsVolume> mount_rw(BlockDevicePtr dev, int* err = nullptr, bool allow_dirty = true);
  const char* fs_name() const override { return "ntfs"; }
  int sync() override;
  // 名前空間の変更 (実験的)。0 / 負の errno。sync() で永続化。
  int create(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino) override;
  int mkdir(uint64_t dir, const std::string& name, uint32_t mode, uint64_t* ino) override;
  int unlink(uint64_t dir, const std::string& name) override;
  int rmdir(uint64_t dir, const std::string& name) override;
  int rename(uint64_t old_dir, const std::string& old_name, uint64_t new_dir, const std::string& new_name) override;
  int64_t write(uint64_t ino, uint64_t off, const void* buf, size_t len) override;   // 実験的: 拡張可。sync() で永続化
  // rw 要求でも読み取り専用に落ちていれば false。落ちる理由: 未再生の $LogFile・休止状態 (どちらもこの層が書き込みを許さない。下の mount_rw) / 上流が見つけた異常 / ビルドで無効。
  bool writable() const override;
  // マウント時の状態 (USBMEM_VOL_DIRTY / LOG_UNCLEAN / HIBERNATED)。読み取り専用で開いたときも返す。
  uint32_t state() const override;
  int truncate(uint64_t ino, uint64_t size) override;   // 実験的: 0 / 負の errno
  int64_t write_inplace(uint64_t ino, uint64_t off, const void* buf, size_t len) override;
  ~NtfsVolume() override;
  uint64_t root() const override { return 5; }
  int lookup(uint64_t dir, const std::string& name, uint64_t* ino) override;
  int stat(uint64_t ino, NtfsStat* st) override;
  int readdir(uint64_t dir, std::vector<NtfsDirEntry>* out) override;
  int64_t read(uint64_t ino, uint64_t off, void* buf, size_t len) override;

 private:
  friend std::unique_ptr<NtfsVolume> mount_impl_(BlockDevicePtr, int, int*, bool);
  static std::unique_ptr<NtfsVolume> mount_once(const BlockDevicePtr& dev, int ro, int* err);   // 1 回マウントし、状態 (state()) も調べる
  NtfsVolume() = default;
  struct Impl;
  std::unique_ptr<Impl> impl_;
};

}  // namespace usbmem
