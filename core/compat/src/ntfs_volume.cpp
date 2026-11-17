// SPDX-License-Identifier: GPL-2.0-or-later
#include "usbmem/ntfs_volume.h"

#include <cerrno>

#include "usbmem/ntfs_c.h"
#include "usbmem/ntfs_logstate.h"

namespace usbmem {

struct NtfsVolume::Impl {
  usbmem_ntfs* v = nullptr;
  usbmem_bdev* bd = nullptr;
  uint32_t state = 0;   // USBMEM_VOL_* (マウント直後)
};

std::unique_ptr<NtfsVolume> mount_impl_(BlockDevicePtr dev, int ro, int* err);
std::unique_ptr<NtfsVolume> NtfsVolume::mount(BlockDevicePtr dev, int* err) { return mount_impl_(std::move(dev), 1, err); }
std::unique_ptr<NtfsVolume> NtfsVolume::mount_rw(BlockDevicePtr dev, int* err) { return mount_impl_(std::move(dev), 0, err); }
int64_t NtfsVolume::write(uint64_t ino, uint64_t off, const void* buf, size_t len) { return usbmem_ntfs_write(impl_->v, ino, off, buf, len); }
int NtfsVolume::create(uint64_t d, const std::string& n, uint32_t m, uint64_t* i) { return usbmem_ntfs_create(impl_->v, d, n.data(), n.size(), m, i); }
int NtfsVolume::mkdir(uint64_t d, const std::string& n, uint32_t m, uint64_t* i) { return usbmem_ntfs_mkdir(impl_->v, d, n.data(), n.size(), m, i); }
int NtfsVolume::unlink(uint64_t d, const std::string& n) { return usbmem_ntfs_unlink(impl_->v, d, n.data(), n.size()); }
int NtfsVolume::rmdir(uint64_t d, const std::string& n) { return usbmem_ntfs_rmdir(impl_->v, d, n.data(), n.size()); }
int NtfsVolume::rename(uint64_t od, const std::string& on, uint64_t nd, const std::string& nn) { return usbmem_ntfs_rename(impl_->v, od, on.data(), on.size(), nd, nn.data(), nn.size()); }
int NtfsVolume::truncate(uint64_t ino, uint64_t size) { return usbmem_ntfs_truncate(impl_->v, ino, size); }
int NtfsVolume::sync() { return usbmem_ntfs_sync(impl_->v); }
int64_t NtfsVolume::write_inplace(uint64_t ino, uint64_t off, const void* buf, size_t len) { return usbmem_ntfs_write_inplace(impl_->v, ino, off, buf, len); }
bool NtfsVolume::writable() const { return usbmem_ntfs_writable(impl_->v) != 0; }
uint32_t NtfsVolume::state() const { return impl_->state; }

// 1 回マウントする (ro != 0 なら読み取り専用)。状態 (ダーティ・休止・未再生ログ) もここで調べて Impl に入れる。
std::unique_ptr<NtfsVolume> NtfsVolume::mount_once(const BlockDevicePtr& dev_in, int ro, int* err) {
  BlockDevicePtr dev = dev_in;
  std::unique_ptr<NtfsVolume> vol(new NtfsVolume());
  vol->impl_.reset(new NtfsVolume::Impl());
  vol->impl_->bd = usbmem_bdev_wrap(&dev);
  int rc = usbmem_ntfs_mount(vol->impl_->bd, ro, &vol->impl_->v);
  if (rc) {
    if (err) *err = -rc;
    usbmem_bdev_free(vol->impl_->bd);
    vol->impl_->bd = nullptr;
    return nullptr;
  }
  uint32_t st = usbmem_ntfs_state(vol->impl_->v);
  // $LogFile の restart page (先頭 2 ページ) が「クリーン」でなければ、Windows が書き込み途中のまま取り外された (または休止した) ボリューム。読むだけ。
  // 読めない / 構造が分からないときは何も立てない (推測しない)。書き込みマウントは上流がマウント中に $LogFile を空にしてしまうので調べない (mount_impl_ が先に読み取り専用で調べている)。
  if (ro) {
    uint8_t pg[8192];
    if (vol->read(2 /* $LogFile */, 0, pg, sizeof pg) == static_cast<int64_t>(sizeof pg) && ntfs_logfile_state(pg, pg + 4096) == NtfsLogState::Unclean) st |= USBMEM_VOL_LOG_UNCLEAN;
  }
  vol->impl_->state = st;
  return vol;
}

std::unique_ptr<NtfsVolume> mount_impl_(BlockDevicePtr dev, int ro, int* err) {
  if (!ro && dev && dev->is_read_only()) ro = 1;  // 下のデバイス (暗号層など) が読み取り専用なら、mount_rw でも読み取り専用でマウントする (ext4 の writable_precheck と同じ)
#ifdef USBMEM_NTFS_WRITE   // 書き込みがビルドで無効なら、従来どおり mount_rw は EROFS で失敗する (下の mount_once)
  if (!ro) {
    // 書き込み可を要求された: 上流の書き込みマウントは、未再生のログがあっても $LogFile を空にして書き始める。ログの再生はしないので、
    // 先に読み取り専用でマウントして状態を調べ、未再生ログ / 休止状態ならそのボリュームを (読み取り専用のまま) 返す。問題なければ、いったん閉じて書き込み可でマウントし直す。
    // 読み取り専用のマウントが失敗したときは何もせず、書き込み可のマウントに進んで従来どおりのエラーコードを返す。
    std::unique_ptr<NtfsVolume> probe = NtfsVolume::mount_once(dev, 1, nullptr);
    if (probe && (probe->impl_->state & (USBMEM_VOL_LOG_UNCLEAN | USBMEM_VOL_HIBERNATED))) return probe;
  }
#endif
  return NtfsVolume::mount_once(dev, ro, err);
}

NtfsVolume::~NtfsVolume() {
  if (!impl_) return;
  usbmem_ntfs_unmount(impl_->v);
  usbmem_bdev_free(impl_->bd);
}

int NtfsVolume::lookup(uint64_t dir, const std::string& name, uint64_t* ino) {
  return usbmem_ntfs_lookup(impl_->v, dir, name.data(), name.size(), ino);
}
int NtfsVolume::stat(uint64_t ino, NtfsStat* st) {
  usbmem_ntfs_stat s;
  int rc = usbmem_ntfs_stat_ino(impl_->v, ino, &s);
  if (rc) return rc;
  st->ino = s.ino; st->size = s.size; st->mode = s.mode; st->nlink = s.nlink; st->mtime = s.mtime_sec;
  return 0;
}
int NtfsVolume::readdir(uint64_t dir, std::vector<NtfsDirEntry>* out) {
  out->clear();
  return usbmem_ntfs_readdir(
      impl_->v, dir,
      [](void* u, const char* n, int len, uint64_t ino, unsigned t) {
        static_cast<std::vector<NtfsDirEntry>*>(u)->push_back({std::string(n, static_cast<size_t>(len)), ino, t});
        return 0;
      },
      out);
}
int64_t NtfsVolume::read(uint64_t ino, uint64_t off, void* buf, size_t len) {
  return usbmem_ntfs_read(impl_->v, ino, off, buf, len);
}
}  // namespace usbmem
