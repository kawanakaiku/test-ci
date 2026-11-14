// SPDX-License-Identifier: Apache-2.0
// BlockTransform の共通部: DecryptingDevice (平文ページキャッシュ + 連続読みの先読み・範囲検査・書き込み拒否・ゼロ化) と probe/open の振り分け。
#include <algorithm>

#include "transform_internal.h"

namespace usbmem {

namespace {
constexpr uint32_t kPageMin = 4096;           // キャッシュの最小ページ (セクタが大きければセクタ)
constexpr size_t kDirectMin = 32 * 1024;      // これ以上のセクタ境界の読みはキャッシュを通さず直接復号する
constexpr size_t kDirectMax = 1u << 20;       // キャッシュを通さない読みの 1 回の上限 (キャンセル確認の粒度)
constexpr uint32_t kRaInitBytes = 16 * 1024;  // 連続読みと判定した最初の先読み量
constexpr uint32_t kRaMaxBytes = 256 * 1024;  // 先読みの上限 (欠けるたびに倍々で増やす)
constexpr size_t kWriteChunk = 1u << 20;      // 書き込みを下位へ渡す 1 回の上限 (一時バッファの大きさ)
}  // namespace

DecryptingDevice::DecryptingDevice(BlockDevicePtr base, uint64_t size, uint32_t sector, uint32_t cache_chunks, std::string desc)
    : base_(std::move(base)),
      size_(size - size % sector),
      sector_(sector),
      page_bytes_(std::max(kPageMin, sector)),
      max_pages_(0),
      desc_(std::move(desc)) {
  // cache_chunks (32 KiB 単位) をページ数に直す。既定は 128 チャンク = 4 MiB。極端に小さい指定でも先読み 1 回分は持てるようにする。
  const uint64_t bytes = uint64_t(cache_chunks ? cache_chunks : 128) * 32 * 1024;
  max_pages_ = uint32_t(std::max<uint64_t>(bytes / page_bytes_, (kRaMaxBytes / page_bytes_) + 2));
}

DecryptingDevice::~DecryptingDevice() {
  // 派生クラスのメンバ (Secret / Aes) は各自のデストラクタでゼロ化される。ここではキャッシュだけ。
  for (Page& c : pages_) c.data.wipe();
  pages_.clear();
}

Status DecryptingDevice::close() {
  std::unique_lock<std::shared_mutex> lk(life_);
  if (closed_) return Status::Ok();
  closed_ = true;
  // 書き込み可なら、最後にもう一度下位へ flush を渡す (呼び出し側の flush 漏れで未反映の書き込みを残さない。失敗は close の結果で返す)。
  Status fs = Status::Ok();
  if (writable_ && !write_failed_.load()) fs = base_->flush();
  {
    std::lock_guard<std::mutex> g(cache_mu_);
    for (Page& c : pages_) c.data.wipe();
    pages_.clear();
    page_map_.clear();
  }
  if (!wiped_) { wiped_ = true; wipe_keys(); }
  return fs;
}

Status DecryptingDevice::flush() {
  if (!writable_) return Status::Ok();
  std::shared_lock<std::shared_mutex> lk(life_);
  if (closed_) return Status::Make(Errc::Closed);
  std::unique_lock<std::shared_mutex> io(io_);  // 進行中の書き込みが下位へ渡り終わってから flush する
  if (write_failed_.load()) return Status::Make(Errc(fail_code_.load()));  // 失敗後は「確実に書けた」と言えない
  Status st = base_->flush();
  if (!st) { fail_code_.store(int(st.code)); write_failed_.store(true); }
  return st;
}

// 連続読みの追跡。first_page が既存ストリームの「次」なら連続 (*sequential = true)、同じページの読み直し (1 KiB の MFT レコードなど) なら
// 連続扱いにはしないが同じストリームとして続ける。どれにも当たらなければ最も古いストリームを置き換える。
uint32_t DecryptingDevice::track_stream_locked(uint64_t first_page, uint64_t last_page, bool* sequential) {
  *sequential = false;
  uint32_t victim = 0;
  for (uint32_t i = 0; i < 4; i++) {
    Stream& s = streams_[i];
    if (s.next != UINT64_MAX && (s.next == first_page || s.next == first_page + 1)) {
      *sequential = (s.next == first_page);
      s.next = last_page + 1;
      s.stamp = ++tick_;
      return i;
    }
    if (s.stamp < streams_[victim].stamp) victim = i;
  }
  Stream& v = streams_[victim];
  v.next = last_page + 1;
  v.ra = 0;
  v.stamp = ++tick_;
  return victim;
}

Status DecryptingDevice::read_at(uint64_t offset, void* buf, size_t len, const IoOptions& opt) {
  std::shared_lock<std::shared_mutex> lk(life_);
  if (closed_) return Status::Make(Errc::Closed);
  if (!range_ok(size_, offset, len)) return Status::Make(Errc::OutOfRange);
  std::shared_lock<std::shared_mutex> io(io_);
  uint8_t* out = static_cast<uint8_t*>(buf);
  uint64_t pos = offset;
  size_t remaining = len;
  bool tracked = false, sequential = false;
  uint32_t stream = 0;
  const uint32_t P = page_bytes_;
  while (remaining) {
    if (opt.cancel && opt.cancel->cancelled()) return Status::Make(Errc::Cancelled);
    if (pos % sector_ == 0 && remaining >= kDirectMin) {
      // 大きい連続読みはキャッシュを通さず直接復号する (キャッシュを押し流さない)
      size_t n = std::min(remaining - remaining % sector_, kDirectMax);
      Status s = read_plain(pos, out, n, opt);
      if (!s) return s;
      pos += n; out += n; remaining -= n;
      continue;
    }
    // キャッシュ経由 (ページ単位)
    const uint64_t idx = pos / P;
    const size_t in_page = size_t(pos % P);
    uint64_t npages;
    {
      std::lock_guard<std::mutex> g(cache_mu_);
      if (!tracked) {
        tracked = true;
        stream = track_stream_locked(idx, (offset + len - 1) / P, &sequential);
      }
      auto it = page_map_.find(idx);
      if (it != page_map_.end()) {
        Page& c = pages_[it->second];
        const size_t take = std::min<size_t>(remaining, P - in_page);
        if (in_page + take > c.len) return Status::Make(Errc::OutOfRange);
        std::memcpy(out, c.data.data() + in_page, take);
        c.stamp = ++tick_;
        pos += take; out += take; remaining -= take;
        continue;
      }
      // 欠けた: 要求に必要なページ (最大 kDirectMin 分) + 連続読みなら先読み を、1 回の下位読み取りで復号する
      const uint64_t last_need = (pos + std::min<size_t>(remaining, kDirectMin) - 1) / P;
      npages = last_need - idx + 1;
      if (sequential) {
        Stream& st = streams_[stream];
        st.ra = st.ra ? std::min(st.ra * 2, kRaMaxBytes / P) : std::max<uint32_t>(kRaInitBytes / P, 1);
        npages = std::max<uint64_t>(npages, st.ra);
      }
      // 既にキャッシュにあるページの手前で打ち切る (読み直さない)
      for (uint64_t j = 1; j < npages; j++)
        if (page_map_.count(idx + j)) { npages = j; break; }
    }
    const uint64_t cstart = idx * P;
    const size_t clen = size_t(std::min<uint64_t>(npages * P, size_ - cstart));
    if (in_page >= clen) return Status::Make(Errc::OutOfRange);
    crypto::Secret tmp(clen);
    Status s = read_plain(cstart, tmp.data(), clen, opt);
    if (!s) return s;
    const size_t take = std::min<size_t>(remaining, clen - in_page);
    std::memcpy(out, tmp.data() + in_page, take);
    {
      std::lock_guard<std::mutex> g(cache_mu_);
      for (size_t o = 0; o < clen; o += P) {
        const uint64_t pi = idx + o / P;
        if (page_map_.count(pi)) continue;  // 別スレッドが先に入れた
        const size_t plen = std::min<size_t>(P, clen - o);
        uint32_t slot;
        if (pages_.size() < max_pages_) {
          pages_.emplace_back();
          slot = uint32_t(pages_.size() - 1);
        } else {
          slot = uint32_t(std::min_element(pages_.begin(), pages_.end(), [](const Page& a, const Page& b) { return a.stamp < b.stamp; }) - pages_.begin());
          auto old = page_map_.find(pages_[slot].index);
          if (old != page_map_.end() && old->second == slot) page_map_.erase(old);  // 無効化済みのスロット (index = UINT64_MAX) は表に無い
          pages_[slot].data.wipe();
        }
        Page& pg = pages_[slot];
        pg.index = pi;
        pg.data = crypto::Secret(tmp.data() + o, plen);
        pg.len = plen;
        pg.stamp = ++tick_;
        page_map_[pi] = slot;
      }
    }
    pos += take; out += take; remaining -= take;
  }
  return Status::Ok();
}


// 書いた範囲に掛かるキャッシュ済みページの内容を、書いた平文で更新する (キャッシュに無いページは入れない)。cache_mu_ は内部で取る。
void DecryptingDevice::update_cache_after_write(uint64_t off, const uint8_t* data, size_t len) {
  const uint32_t P = page_bytes_;
  std::lock_guard<std::mutex> g(cache_mu_);
  if (page_map_.empty()) return;
  for (uint64_t idx = off / P; idx <= (off + len - 1) / P; idx++) {
    auto it = page_map_.find(idx);
    if (it == page_map_.end()) continue;
    Page& c = pages_[it->second];
    const uint64_t pstart = idx * P;
    const uint64_t a = std::max<uint64_t>(off, pstart), b = std::min<uint64_t>(off + len, pstart + c.len);
    if (a < b) std::memcpy(c.data.data() + (a - pstart), data + (a - off), size_t(b - a));
  }
}

// 範囲に掛かるキャッシュ済みページを捨てる (書き込みが失敗して下位の内容が不確かなとき)。スロットは最古扱いにして再利用させる。
void DecryptingDevice::invalidate_pages(uint64_t off, size_t len) {
  const uint32_t P = page_bytes_;
  std::lock_guard<std::mutex> g(cache_mu_);
  if (page_map_.empty()) return;
  for (uint64_t idx = off / P; idx <= (off + len - 1) / P; idx++) {
    auto it = page_map_.find(idx);
    if (it == page_map_.end()) continue;
    Page& c = pages_[it->second];
    c.data.wipe();
    c.len = 0;
    c.index = UINT64_MAX;
    c.stamp = 0;
    page_map_.erase(it);
  }
}

Status DecryptingDevice::write_at(uint64_t offset, const void* buf, size_t len, const IoOptions& opt) {
  if (!writable_) return Status::Make(Errc::ReadOnly);
  std::shared_lock<std::shared_mutex> lk(life_);
  if (closed_) return Status::Make(Errc::Closed);
  if (write_failed_.load()) return Status::Make(Errc::ReadOnly);
  if (!range_ok(size_, offset, len)) return Status::Make(Errc::OutOfRange);
  if (len == 0) return Status::Ok();
  std::unique_lock<std::shared_mutex> io(io_);  // 読み取りとも他の書き込みとも排他 (RMW の原子性・キャッシュの整合)
  if (write_failed_.load()) return Status::Make(Errc::ReadOnly);
  Status rs = check_write_range(offset, len);  // 要求全体を最初に検査する (途中まで書いて拒否、にしない)
  if (!rs) return rs;
  if (opt.cancel && opt.cancel->cancelled()) return Status::Make(Errc::Cancelled);
  const uint8_t* in = static_cast<const uint8_t*>(buf);
  const uint64_t total_end = offset + len;
  const uint32_t S = sector_;
  uint64_t pos = offset;
  bool wrote = false;  // 下位へ 1 回でも書いた (以後の失敗は途中まで反映されたことになる)
  while (pos < total_end) {
    uint64_t end = std::min<uint64_t>(total_end, pos + kWriteChunk);
    if (end != total_end) end -= end % S;  // 最終でない塊は暗号セクタ境界で切る (同じセクタを 2 回書かない)
    const uint64_t a0 = pos - pos % S;
    const uint64_t a1 = (end + S - 1) / S * S;  // size_ は S の倍数なので範囲内
    crypto::Secret plain(size_t(a1 - a0));
    Status st = Status::Ok();
    bool head_loaded = false;
    if (a0 != pos) { st = read_plain(a0, plain.data(), S, opt); head_loaded = true; }  // 先頭の部分セクタ
    if (st && a1 != end && !(head_loaded && a1 - a0 == S)) st = read_plain(a1 - S, plain.data() + (a1 - a0 - S), S, opt);  // 末尾の部分セクタ
    if (!st) {
      if (wrote) { fail_code_.store(int(st.code)); write_failed_.store(true); }
      return st;
    }
    std::memcpy(plain.data() + (pos - a0), in + (pos - offset), size_t(end - pos));
    st = write_plain(a0, plain.data(), plain.size(), opt);
    if (!st) {
      invalidate_pages(a0, plain.size());  // 下位の内容は新旧が混ざっているかもしれない
      fail_code_.store(int(st.code));
      write_failed_.store(true);
      return st;
    }
    wrote = true;
    update_cache_after_write(a0, plain.data(), plain.size());
    pos = end;
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
