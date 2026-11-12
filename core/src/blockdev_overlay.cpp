// SPDX-License-Identifier: Apache-2.0
// COW オーバーレイ。ベースは読むだけ。書き込みは block_size 単位で、メモリまたは追記ログファイルに溜める。
//
// ログファイル形式 (リトルエンディアン):
//   ヘッダ 64 B: "USBMOVL1" | block_size u32 | reserved u32 | base_size u64 | ... | crc32(先頭 60 B) u32
//   レコード   : "REC1" u32 | block_index u64 | data[block_size] | crc32(block_index(8) + data) u32
// 同じブロックの後のレコードが勝つ。末尾が切れている / CRC が合わないレコードに出会ったらそこで読み込みを止め、
// 以降を (ファイルとしても) 切り捨てる。= クラッシュ時は「最後に完全に書けたレコードまで」に戻る。
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <mutex>
#include <unordered_map>

#include "blockdev_internal.h"
#include "usbmem/crc32.h"

namespace usbmem {

namespace {
constexpr size_t kHdr = 64;
constexpr uint32_t kRecMagic = 0x31434552;  // "REC1" (LE)
void put32(uint8_t* p, uint32_t v) { for (int i = 0; i < 4; i++) p[i] = static_cast<uint8_t>(v >> (8 * i)); }
void put64(uint8_t* p, uint64_t v) { for (int i = 0; i < 8; i++) p[i] = static_cast<uint8_t>(v >> (8 * i)); }

Status pread_all(int fd, void* buf, size_t n, uint64_t off) {
  uint8_t* p = static_cast<uint8_t*>(buf);
  while (n > 0) {
    ssize_t r = ::pread(fd, p, n, static_cast<off_t>(off));
    if (r < 0) { if (errno == EINTR) continue; return Status::Make(Errc::Io, errno); }
    if (r == 0) return Status::Make(Errc::Io, 0);
    p += r; off += static_cast<uint64_t>(r); n -= static_cast<size_t>(r);
  }
  return Status::Ok();
}
Status pwrite_all(int fd, const void* buf, size_t n, uint64_t off) {
  const uint8_t* p = static_cast<const uint8_t*>(buf);
  while (n > 0) {
    ssize_t r = ::pwrite(fd, p, n, static_cast<off_t>(off));
    if (r < 0) { if (errno == EINTR) continue; return Status::Make(Errc::Io, errno); }
    p += r; off += static_cast<uint64_t>(r); n -= static_cast<size_t>(r);
  }
  return Status::Ok();
}
}  // namespace

struct OverlayDevice::Impl {
  std::mutex mu;
  bool closed = false;
  int fd = -1;
  bool sync_on_flush = true;
  uint64_t max_bytes = 0;
  uint64_t log_end = kHdr;                          // 次のレコードの書き込み位置
  std::unordered_map<uint64_t, uint64_t> file_idx;  // block -> データのファイル内オフセット (ファイルモード)
  std::unordered_map<uint64_t, std::vector<uint8_t>> mem;  // メモリモード
  size_t rec_size = 0;
};

OverlayDevice::OverlayDevice(BlockDevicePtr base, uint32_t bs)
    : impl_(new Impl), base_(std::move(base)), size_(base_->size()), block_size_(bs) {
  impl_->rec_size = 4 + 8 + bs + 4;
}

OverlayDevice::~OverlayDevice() { close(); }

Status OverlayDevice::create(BlockDevicePtr base, const Options& opt, std::shared_ptr<OverlayDevice>* out) {
  if (!base || !out) return Status::Make(Errc::InvalidArgument);
  uint32_t bs = opt.block_size;
  if (bs < 512 || bs > (4u << 20) || !imgint::is_pow2(bs) || bs % base->logical_block_size() != 0) return Status::Make(Errc::InvalidArgument);
  std::shared_ptr<OverlayDevice> dev(new OverlayDevice(base, bs));
  Impl& im = *dev->impl_;
  im.sync_on_flush = opt.sync_on_flush;
  im.max_bytes = opt.max_overlay_bytes;
  if (!opt.file_path.empty()) {
    int fd = ::open(opt.file_path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (fd < 0) return Status::Make(Errc::Io, errno);
    im.fd = fd;
    struct stat stt;
    if (fstat(fd, &stt) != 0) return Status::Make(Errc::Io, errno);
    uint64_t fsz = static_cast<uint64_t>(stt.st_size);
    uint8_t hdr[kHdr] = {0};
    if (fsz == 0) {
      std::memcpy(hdr, "USBMOVL1", 8);
      put32(hdr + 8, bs);
      put64(hdr + 16, base->size());
      put32(hdr + 60, crc32(hdr, 60));
      Status st = pwrite_all(fd, hdr, kHdr, 0);
      if (!st.ok()) return st;
    } else {
      if (fsz < kHdr) return Status::Make(Errc::Corrupt);
      Status st = pread_all(fd, hdr, kHdr, 0);
      if (!st.ok()) return st;
      if (std::memcmp(hdr, "USBMOVL1", 8) != 0 || imgint::le32(hdr + 60) != crc32(hdr, 60)) return Status::Make(Errc::Corrupt);
      if (imgint::le32(hdr + 8) != bs || imgint::le64(hdr + 16) != base->size()) return Status::Make(Errc::InvalidArgument);  // 別のベース/設定用のログ
      // 再生: 完全で CRC が合うレコードだけ採用
      std::vector<uint8_t> rec(im.rec_size);
      uint64_t pos = kHdr;
      while (pos + im.rec_size <= fsz) {
        st = pread_all(fd, rec.data(), rec.size(), pos);
        if (!st.ok()) return st;
        if (imgint::le32(rec.data()) != kRecMagic) break;
        uint32_t want = imgint::le32(rec.data() + 12 + bs);
        if (crc32(rec.data() + 4, 8 + bs) != want) break;
        uint64_t bi = imgint::le64(rec.data() + 4);
        if (bi >= (dev->size_ + bs - 1) / bs) break;
        im.file_idx[bi] = pos + 12;
        pos += im.rec_size;
      }
      im.log_end = pos;
      if (pos != fsz && ftruncate(fd, static_cast<off_t>(pos)) != 0) return Status::Make(Errc::Io, errno);  // 壊れた末尾を捨てる
    }
  }
  *out = std::move(dev);
  return Status::Ok();
}

uint64_t OverlayDevice::dirty_blocks() const {
  std::lock_guard<std::mutex> lk(impl_->mu);
  return impl_->fd >= 0 ? impl_->file_idx.size() : impl_->mem.size();
}

std::string OverlayDevice::describe() const { return "overlay(" + base_->describe() + ")"; }

namespace {
// ブロック bi の現在の内容 (オーバーレイ優先、なければベース) を buf[bs] に読む。末尾ブロックの範囲外は 0。
}

Status OverlayDevice::read_at(uint64_t off, void* buf, size_t len, const IoOptions& opt) {
  Impl& im = *impl_;
  std::lock_guard<std::mutex> lk(im.mu);
  if (im.closed) return Status::Make(Errc::Closed);
  if (!range_ok(size_, off, len)) return Status::Make(Errc::OutOfRange);
  uint8_t* p = static_cast<uint8_t*>(buf);
  while (len > 0) {
    if (opt.cancel && opt.cancel->cancelled()) return Status::Make(Errc::Cancelled);
    uint64_t bi = off / block_size_, in = off % block_size_;
    size_t n = static_cast<size_t>(std::min<uint64_t>(len, block_size_ - in));
    bool have = false;
    if (im.fd >= 0) {
      auto it = im.file_idx.find(bi);
      if (it != im.file_idx.end()) { Status st = pread_all(im.fd, p, n, it->second + in); if (!st.ok()) return st; have = true; }
    } else {
      auto it = im.mem.find(bi);
      if (it != im.mem.end()) { std::memcpy(p, it->second.data() + in, n); have = true; }
    }
    if (!have) { Status st = base_->read_at(off, p, n, opt); if (!st.ok()) return st; }
    p += n; off += n; len -= n;
  }
  return Status::Ok();
}

Status OverlayDevice::write_at(uint64_t off, const void* buf, size_t len, const IoOptions& opt) {
  Impl& im = *impl_;
  std::lock_guard<std::mutex> lk(im.mu);
  if (im.closed) return Status::Make(Errc::Closed);
  if (!range_ok(size_, off, len)) return Status::Make(Errc::OutOfRange);
  const uint8_t* p = static_cast<const uint8_t*>(buf);
  std::vector<uint8_t> blk(block_size_), rec(im.rec_size);
  while (len > 0) {
    if (opt.cancel && opt.cancel->cancelled()) return Status::Make(Errc::Cancelled);
    uint64_t bi = off / block_size_, in = off % block_size_;
    size_t n = static_cast<size_t>(std::min<uint64_t>(len, block_size_ - in));
    uint64_t bstart = bi * block_size_;
    size_t valid = static_cast<size_t>(std::min<uint64_t>(block_size_, size_ - bstart));
    bool full = (n == block_size_);
    // 現在の内容を用意 (部分書き込みなら RMW)
    bool exists = false;
    if (im.fd >= 0) exists = im.file_idx.count(bi) != 0; else exists = im.mem.count(bi) != 0;
    if (!full) {
      std::fill(blk.begin(), blk.end(), 0);
      if (exists) {
        if (im.fd >= 0) { Status st = pread_all(im.fd, blk.data(), block_size_, im.file_idx[bi]); if (!st.ok()) return st; }
        else blk = im.mem[bi];
      } else {
        Status st = base_->read_at(bstart, blk.data(), valid, opt);
        if (!st.ok()) return st;
      }
    }
    std::memcpy(blk.data() + in, p, n);
    if (im.fd >= 0) {
      if (!exists && im.max_bytes && (im.file_idx.size() + 1) * block_size_ > im.max_bytes) return Status::Make(Errc::Io, ENOSPC);
      put32(rec.data(), kRecMagic);
      put64(rec.data() + 4, bi);
      std::memcpy(rec.data() + 12, blk.data(), block_size_);
      put32(rec.data() + 12 + block_size_, crc32(rec.data() + 4, 8 + block_size_));
      Status st = pwrite_all(im.fd, rec.data(), rec.size(), im.log_end);
      if (!st.ok()) return st;
      im.file_idx[bi] = im.log_end + 12;
      im.log_end += rec.size();
    } else {
      if (!exists && im.max_bytes && (im.mem.size() + 1) * block_size_ > im.max_bytes) return Status::Make(Errc::Io, ENOSPC);
      im.mem[bi] = blk;
    }
    p += n; off += n; len -= n;
  }
  return Status::Ok();
}

Status OverlayDevice::flush() {
  Impl& im = *impl_;
  std::lock_guard<std::mutex> lk(im.mu);
  if (im.closed) return Status::Make(Errc::Closed);
  if (im.fd >= 0 && im.sync_on_flush && fdatasync(im.fd) != 0) return Status::Make(Errc::Io, errno);
  return Status::Ok();
}

Status OverlayDevice::close() {
  Impl& im = *impl_;
  std::lock_guard<std::mutex> lk(im.mu);
  if (im.closed) return Status::Ok();
  im.closed = true;
  if (im.fd >= 0) { ::close(im.fd); im.fd = -1; }
  im.mem.clear();
  im.file_idx.clear();
  return base_->close();
}

Status OverlayDevice::commit(BlockDevice& target, bool clear_after, const IoOptions& opt) {
  Impl& im = *impl_;
  std::lock_guard<std::mutex> lk(im.mu);
  if (im.closed) return Status::Make(Errc::Closed);
  if (target.is_read_only()) return Status::Make(Errc::ReadOnly);
  if (target.size() < size_) return Status::Make(Errc::InvalidArgument);
  std::vector<uint64_t> keys;
  if (im.fd >= 0) for (auto& kv : im.file_idx) keys.push_back(kv.first); else for (auto& kv : im.mem) keys.push_back(kv.first);
  std::sort(keys.begin(), keys.end());   // 昇順 (シーケンシャルに書く)
  std::vector<uint8_t> blk(block_size_);
  for (uint64_t bi : keys) {
    if (opt.cancel && opt.cancel->cancelled()) return Status::Make(Errc::Cancelled);
    uint64_t bstart = bi * block_size_;
    size_t valid = static_cast<size_t>(std::min<uint64_t>(block_size_, size_ - bstart));
    if (im.fd >= 0) { Status st = pread_all(im.fd, blk.data(), block_size_, im.file_idx[bi]); if (!st.ok()) return st; }
    else blk = im.mem[bi];
    Status st = target.write_at(bstart, blk.data(), valid, opt);
    if (!st.ok()) return st;
  }
  Status st = target.flush();
  if (!st.ok()) return st;
  if (clear_after) {
    im.mem.clear();
    im.file_idx.clear();
    if (im.fd >= 0) {
      if (ftruncate(im.fd, static_cast<off_t>(kHdr)) != 0) return Status::Make(Errc::Io, errno);
      im.log_end = kHdr;
    }
  }
  return Status::Ok();
}

Status OverlayDevice::discard() {
  Impl& im = *impl_;
  std::lock_guard<std::mutex> lk(im.mu);
  if (im.closed) return Status::Make(Errc::Closed);
  im.mem.clear();
  im.file_idx.clear();
  if (im.fd >= 0) {
    if (ftruncate(im.fd, static_cast<off_t>(kHdr)) != 0) return Status::Make(Errc::Io, errno);
    im.log_end = kHdr;
  }
  return Status::Ok();
}

}  // namespace usbmem
