// SPDX-License-Identifier: GPL-2.0-or-later
#include "usbmem/ntfs_volume.h"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <vector>

#include "usbmem/ntfs_c.h"
#include "usbmem/ntfs_logstate.h"

namespace usbmem {

// ---- torn write などで壊れた MFT システムレコード (0-3: $MFT / $MFTMirr / $LogFile / $Volume) を $MFTMirr のコピーで読み出す緩和 (読み取り専用のみ。docs/ntfs-mftmirr.md)。
// 上流ドライバは、これらのレコードのマルチセクタ保護 (fixup) が食い違うとマウントできない。$MFTMirr が無傷なら、壊れたレコードの位置の読み取りだけを
// $MFTMirr の同じ番号のレコードに差し替えた読み取り専用のデバイス越しにマウントする。書き込みは一切通さず、ログ・ダーティフラグにも触らない。
// 通常のマウントが失敗したときだけ試す (健全なボリュームは従来どおり、余計な読み取りもしない)。
namespace {

uint16_t rd16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }
uint32_t rd32(const uint8_t* p) { return static_cast<uint32_t>(p[0]) | (static_cast<uint32_t>(p[1]) << 8) | (static_cast<uint32_t>(p[2]) << 16) | (static_cast<uint32_t>(p[3]) << 24); }
uint64_t rd64(const uint8_t* p) { return static_cast<uint64_t>(rd32(p)) | (static_cast<uint64_t>(rd32(p + 4)) << 32); }

// MFT レコード rec (rs バイト、ディスク上の生の内容) が使えるか: FILE の印、fixup 配列の位置と個数、全セクタ末尾の USN の一致、使用量・割り当て量。
// n は期待するレコード番号 (NTFS 3.1 の 0x30 から始まる形式だけ、レコード内の番号欄 0x2C も照合する)。
bool mft_record_sound(const uint8_t* rec, size_t rs, uint32_t n) {
  if (std::memcmp(rec, "FILE", 4) != 0) return false;
  uint16_t ufo = rd16(rec + 4), ufc = rd16(rec + 6);
  if (rs % 512 != 0 || ufc != rs / 512 + 1 || (ufo & 1) || ufo < 0x2A || static_cast<size_t>(ufo) + 2u * ufc > rs) return false;
  uint16_t usn = rd16(rec + ufo);
  for (size_t i = 1; i < ufc; i++) if (rd16(rec + 512 * i - 2) != usn) return false;
  if (rd32(rec + 0x1C) != rs || rd32(rec + 0x18) > rs || rd32(rec + 0x18) < 0x30) return false;
  if (ufo >= 0x30 && rd32(rec + 0x2C) != n) return false;
  return true;
}

class MirrorOverlayDevice final : public BlockDevice {
 public:
  struct Patch { uint64_t off; std::vector<uint8_t> data; };
  MirrorOverlayDevice(BlockDevicePtr base, std::vector<Patch> patches) : base_(std::move(base)), patches_(std::move(patches)) {}
  Status read_at(uint64_t offset, void* buf, size_t len, const IoOptions& opt = {}) override {
    Status st = base_->read_at(offset, buf, len, opt);
    if (!st.ok()) return st;
    for (const Patch& p : patches_) {
      uint64_t lo = std::max<uint64_t>(offset, p.off), hi = std::min<uint64_t>(offset + len, p.off + p.data.size());
      if (lo < hi) std::memcpy(static_cast<uint8_t*>(buf) + (lo - offset), p.data.data() + (lo - p.off), static_cast<size_t>(hi - lo));
    }
    return st;
  }
  Status write_at(uint64_t, const void*, size_t, const IoOptions& = {}) override { return Status::Make(Errc::ReadOnly); }
  Status flush() override { return Status::Ok(); }
  Status close() override { return Status::Ok(); }   // 下位は所有者が閉じる
  uint64_t size() const override { return base_->size(); }
  uint32_t logical_block_size() const override { return base_->logical_block_size(); }
  bool is_read_only() const override { return true; }
  std::string describe() const override { return "ntfs-mftmirr-overlay(" + base_->describe() + ")"; }

 private:
  BlockDevicePtr base_;
  std::vector<Patch> patches_;
};

// 先頭 4 レコードのうち、$MFT 側が壊れていて $MFTMirr 側が使えるものを差し替えるデバイスを作る。差し替えるものが無ければ nullptr。
BlockDevicePtr make_mirror_overlay(const BlockDevicePtr& dev) {
  uint8_t bs[512];
  if (dev->size() < sizeof bs || !dev->read_at(0, bs, sizeof bs).ok()) return nullptr;
  if (std::memcmp(bs + 3, "NTFS    ", 8) != 0) return nullptr;
  uint32_t bps = rd16(bs + 11);
  uint32_t spc = bs[13];
  if (spc > 0x80) spc = 1u << (256 - spc);   // 負の値 (2 の冪の指数)
  uint64_t cs = static_cast<uint64_t>(bps) * spc;
  int8_t cpr = static_cast<int8_t>(bs[0x40]);
  uint64_t rs = cpr > 0 ? static_cast<uint64_t>(cpr) * cs : (cpr > -32 ? 1ull << (-cpr) : 0);
  if (bps < 512 || cs == 0 || cs > (1u << 21) || rs < 1024 || rs > 65536 || (rs & 511)) return nullptr;
  uint64_t mft = rd64(bs + 0x30) * cs, mirr = rd64(bs + 0x38) * cs;
  if (mft == 0 || mirr == 0 || mft + 4 * rs > dev->size() || mirr + 4 * rs > dev->size()) return nullptr;
  std::vector<uint8_t> a(4 * rs), b(4 * rs);
  if (!dev->read_at(mft, a.data(), a.size()).ok() || !dev->read_at(mirr, b.data(), b.size()).ok()) return nullptr;
  std::vector<MirrorOverlayDevice::Patch> patches;
  for (uint32_t n = 0; n < 4; n++) {
    const uint8_t* pa = a.data() + n * rs;
    const uint8_t* pb = b.data() + n * rs;
    if (mft_record_sound(pa, rs, n) || !mft_record_sound(pb, rs, n)) continue;   // $MFT 側が無傷 / ミラー側も使えない: 差し替えない
    patches.push_back({mft + n * rs, std::vector<uint8_t>(pb, pb + rs)});
  }
  if (patches.empty()) return nullptr;
  return std::make_shared<MirrorOverlayDevice>(dev, std::move(patches));
}

}  // namespace

struct NtfsVolume::Impl {
  usbmem_ntfs* v = nullptr;
  usbmem_bdev* bd = nullptr;
  uint32_t state = 0;   // USBMEM_VOL_* (マウント直後)
};

std::unique_ptr<NtfsVolume> mount_impl_(BlockDevicePtr dev, int ro, int* err, bool allow_dirty);
std::unique_ptr<NtfsVolume> NtfsVolume::mount(BlockDevicePtr dev, int* err) { return mount_impl_(std::move(dev), 1, err, true); }
std::unique_ptr<NtfsVolume> NtfsVolume::mount_rw(BlockDevicePtr dev, int* err, bool allow_dirty) { return mount_impl_(std::move(dev), 0, err, allow_dirty); }
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

std::unique_ptr<NtfsVolume> mount_impl_(BlockDevicePtr dev, int ro, int* err, bool allow_dirty) {
  if (!ro && dev && dev->is_read_only()) ro = 1;  // 下のデバイス (暗号層など) が読み取り専用なら、mount_rw でも読み取り専用でマウントする (ext4 の writable_precheck と同じ)
#ifdef USBMEM_NTFS_WRITE   // 書き込みがビルドで無効なら、従来どおり mount_rw は EROFS で失敗する (下の mount_once)
  if (!ro) {
    // 書き込み可を要求された: 上流の書き込みマウントは、未再生のログがあっても $LogFile を空にして書き始める。ログの再生はしないので、
    // 先に読み取り専用でマウントして状態を調べ、未再生ログ / 休止状態ならそのボリュームを (読み取り専用のまま) 返す。問題なければ、いったん閉じて書き込み可でマウントし直す。
    // 読み取り専用のマウントが失敗したときは何もせず、書き込み可のマウントに進んで従来どおりのエラーコードを返す。
    std::unique_ptr<NtfsVolume> probe = NtfsVolume::mount_once(dev, 1, nullptr);
    if (probe && (probe->impl_->state & (USBMEM_VOL_LOG_UNCLEAN | USBMEM_VOL_HIBERNATED))) return probe;
    // ダーティフラグだけ (ログは静止): 呼び出し側が確認するまで書き込みマウントしない (上流の書き込みマウントはフラグを立て直し、閉じるときに下ろす)。読み取り専用のまま返す
    if (probe && !allow_dirty && (probe->impl_->state & USBMEM_VOL_DIRTY)) { probe->impl_->state |= USBMEM_VOL_CONFIRM_WRITE; return probe; }
  }
#endif
  int err1 = 0;
  std::unique_ptr<NtfsVolume> vol = NtfsVolume::mount_once(dev, ro, &err1);
  if (!vol) {
    // マウントできない。先頭 4 レコードの $MFT 側が壊れていて $MFTMirr が無傷なら、$MFTMirr のコピーで読み取り専用に開く (書き込み可を要求されていても読み取り専用)
    if (BlockDevicePtr ov = make_mirror_overlay(dev)) {
      int err2 = 0;
      vol = NtfsVolume::mount_once(ov, 1, &err2);
      if (vol) { vol->impl_->state |= USBMEM_VOL_MFT_FROM_MIRROR; return vol; }
    }
    if (err) *err = err1;
  }
  return vol;
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
