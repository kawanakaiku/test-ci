// SPDX-License-Identifier: Apache-2.0
// BlockTransform の共通部: DecryptingDevice (平文チャンクキャッシュ・範囲検査・書き込み拒否・ゼロ化) と probe/open の振り分け。
#include <algorithm>

#include "transform_internal.h"

namespace usbmem {

namespace {
constexpr uint32_t kChunkBytes = 32 * 1024;
constexpr size_t kDirectMax = 1u << 20;  // キャッシュを通さない読みの 1 回の上限 (キャンセル確認の粒度)
}  // namespace

DecryptingDevice::DecryptingDevice(BlockDevicePtr base, uint64_t size, uint32_t sector, uint32_t cache_chunks, std::string desc)
    : base_(std::move(base)),
      size_(size - size % sector),
      sector_(sector),
      chunk_bytes_(std::max(kChunkBytes, sector)),
      max_chunks_(cache_chunks ? cache_chunks : 32),
      desc_(std::move(desc)) {}

DecryptingDevice::~DecryptingDevice() {
  // 派生クラスのメンバ (Secret / Aes) は各自のデストラクタでゼロ化される。ここではキャッシュだけ。
  for (Chunk& c : cache_) c.data.wipe();
  cache_.clear();
}

Status DecryptingDevice::close() {
  std::unique_lock<std::shared_mutex> lk(life_);
  if (closed_) return Status::Ok();
  closed_ = true;
  {
    std::lock_guard<std::mutex> g(cache_mu_);
    for (Chunk& c : cache_) c.data.wipe();
    cache_.clear();
  }
  if (!wiped_) { wiped_ = true; wipe_keys(); }
  return Status::Ok();
}

Status DecryptingDevice::read_at(uint64_t offset, void* buf, size_t len, const IoOptions& opt) {
  std::shared_lock<std::shared_mutex> lk(life_);
  if (closed_) return Status::Make(Errc::Closed);
  if (!range_ok(size_, offset, len)) return Status::Make(Errc::OutOfRange);
  uint8_t* out = static_cast<uint8_t*>(buf);
  uint64_t pos = offset;
  size_t remaining = len;
  while (remaining) {
    if (opt.cancel && opt.cancel->cancelled()) return Status::Make(Errc::Cancelled);
    const uint64_t idx = pos / chunk_bytes_;
    const size_t in_chunk = size_t(pos % chunk_bytes_);
    if (pos % sector_ == 0 && remaining >= chunk_bytes_) {
      // 大きい連続読みはキャッシュを通さず直接復号する (キャッシュを押し流さない)
      size_t n = std::min(remaining - remaining % sector_, kDirectMax);
      Status s = read_plain(pos, out, n, opt);
      if (!s) return s;
      pos += n; out += n; remaining -= n;
      continue;
    }
    // キャッシュ経由
    const size_t take = std::min<size_t>(remaining, chunk_bytes_ - in_chunk);
    bool hit = false;
    {
      std::lock_guard<std::mutex> g(cache_mu_);
      for (Chunk& c : cache_)
        if (c.index == idx) {
          if (in_chunk + take > c.len) return Status::Make(Errc::OutOfRange);
          std::memcpy(out, c.data.data() + in_chunk, take);
          c.stamp = ++tick_;
          hit = true;
          break;
        }
    }
    if (!hit) {
      const uint64_t cstart = idx * chunk_bytes_;
      const size_t clen = size_t(std::min<uint64_t>(chunk_bytes_, size_ - cstart));
      crypto::Secret tmp(clen);
      Status s = read_plain(cstart, tmp.data(), clen, opt);
      if (!s) return s;
      if (in_chunk + take > clen) return Status::Make(Errc::OutOfRange);
      std::memcpy(out, tmp.data() + in_chunk, take);
      std::lock_guard<std::mutex> g(cache_mu_);
      Chunk* slot = nullptr;
      if (cache_.size() < max_chunks_) {
        cache_.push_back(Chunk{idx, crypto::Secret(), 0, 0});
        slot = &cache_.back();
      } else {
        slot = &*std::min_element(cache_.begin(), cache_.end(), [](const Chunk& a, const Chunk& b) { return a.stamp < b.stamp; });
        slot->data.wipe();
      }
      slot->index = idx;
      slot->data = std::move(tmp);
      slot->len = clen;
      slot->stamp = ++tick_;
    }
    pos += take; out += take; remaining -= take;
  }
  return Status::Ok();
}

namespace tf {
Status read_exact(BlockDevice& dev, uint64_t off, void* buf, size_t len) {
  if (!range_ok(dev.size(), off, len)) return Status::Make(Errc::OutOfRange);
  return dev.read_at(off, buf, len);
}
}  // namespace tf

Status probe_crypto_volume(BlockDevice& dev, VolumeInfo* info) {
  VolumeInfo tmp;
  VolumeInfo* out = info ? info : &tmp;
  *out = VolumeInfo();
  Status s = tf::bitlocker_probe(dev, out);
  if (s.code != Errc::Unsupported || out->kind != CryptoKind::Auto) return s;
  s = tf::luks_probe(dev, out);
  return s;
}

Status open_crypto_volume(BlockDevicePtr base, CryptoKind kind, const Credentials& cred, const OpenOptions& opt, BlockDevicePtr* out,
                          VolumeInfo* info) {
  if (!out) return Status::Make(Errc::InvalidArgument);
  *out = nullptr;
  if (!base) return Status::Make(Errc::InvalidArgument);
  VolumeInfo local;
  VolumeInfo* inf = info ? info : &local;
  *inf = VolumeInfo();
  if (kind == CryptoKind::Auto) {
    Status s = probe_crypto_volume(*base, inf);
    if (!s && inf->kind == CryptoKind::Auto) return s;  // 暗号ボリュームではない / 読めない
    kind = inf->kind;
    if (kind == CryptoKind::Auto) return Status::Make(Errc::Unsupported);
  }
  switch (kind) {
    case CryptoKind::BitLocker: return tf::bitlocker_open(std::move(base), cred, opt, out, inf);
    case CryptoKind::Luks: return tf::luks_open(std::move(base), cred, opt, out, inf);
    default: return Status::Make(Errc::InvalidArgument);
  }
}

}  // namespace usbmem
