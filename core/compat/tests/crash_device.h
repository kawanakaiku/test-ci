// SPDX-License-Identifier: GPL-2.0-or-later
// 電源断の模擬デバイス (test_ntfs.cpp の CrashDevice と同じ実装)。tools/run_ext2_crash_tests.sh が使う。
#pragma once
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <random>
#include <vector>

#include "usbmem/block_device.h"

// 電源断の模擬: write_at を数え、N 回目で「電源が落ちる」。以降の書き込みは消える (読み出しは通常どおり。元のデータを返す)。
//   prefix モード: 書き込みは発行順に即時反映 (順序を守るデバイス)。cache モード: flush までバッファし、電源断の瞬間に未 flush の書き込みの
//   ランダムな部分集合だけが反映される (順序保証なしの USB キャッシュ)。torn=1 なら電源断時に飛んでくる 1 回の書き込みをセクタ途中で切る。
class CrashDevice : public usbmem::BlockDevice {
 public:
  CrashDevice(usbmem::BlockDevicePtr p, long limit, bool cache, bool torn, unsigned seed) : p_(std::move(p)), limit_(limit), cache_(cache), torn_(torn), rng_(seed) {}
  usbmem::Status read_at(uint64_t off, void* buf, size_t len, const usbmem::IoOptions& o = {}) override {
    // 未 flush のバッファ分も (電源が入っている間は) ホストから見える: キャッシュのある実デバイスと同じ
    usbmem::Status st = p_->read_at(off, buf, len, o);
    if (!st.ok() || !cache_) return st;
    for (auto& w : pend_) {
      uint64_t a = std::max<uint64_t>(off, w.off), b = std::min<uint64_t>(off + len, w.off + w.data.size());
      if (a < b) std::memcpy((char*)buf + (a - off), w.data.data() + (a - w.off), b - a);
    }
    return st;
  }
  usbmem::Status write_at(uint64_t off, const void* buf, size_t len, const usbmem::IoOptions& o = {}) override {
    if (dead_) return usbmem::Status::Ok();
    if (limit_ >= 0 && count_ >= limit_) { die(off, buf, len); return usbmem::Status::Ok(); }
    count_++;
    if (std::getenv("USBMEM_WRITE_LOG")) std::fprintf(stderr, "W%ld off=0x%llx len=%zu\n", count_, (unsigned long long)off, len);
    if (cache_) { pend_.push_back({off, std::vector<uint8_t>((const uint8_t*)buf, (const uint8_t*)buf + len)}); return usbmem::Status::Ok(); }
    return p_->write_at(off, buf, len, o);
  }
  usbmem::Status flush() override {
    if (dead_) return usbmem::Status::Ok();
    // USBMEM_CRASH_FLUSH_CUTS=1 (cache 方式のみ): flush の呼び出しも電源断のカットポイントに数える。flush の直前 (未 flush の書き込みがまだ全部バッファにある瞬間) に切れる。
    // 既定では書き込みの前でしか切れないので、「最後の書き込みと flush の間」や「flush を省いた順序」の電源断が試せない (XFS の突然変異テストが使う。他の FS のテストは変わらない)。
    if (cache_ && flush_cuts_) {
      if (limit_ >= 0 && count_ >= limit_) { die(0, nullptr, 0); return usbmem::Status::Ok(); }
      count_++;
    }
    if (std::getenv("USBMEM_WRITE_LOG")) std::fprintf(stderr, "F (after %ld writes)\n", count_);
    for (auto& w : pend_) p_->write_at(w.off, w.data.data(), w.data.size());
    pend_.clear(); return p_->flush();
  }
  usbmem::Status close() override { return p_->close(); }
  uint64_t size() const override { return p_->size(); }
  uint32_t logical_block_size() const override { return p_->logical_block_size(); }
  bool is_read_only() const override { return false; }
  std::string describe() const override { return "crash(" + p_->describe() + ")"; }
  long count() const { return count_; }
  bool dead() const { return dead_; }   // 電源断が起きた (以後の書き込みは消える)
 private:
  struct W { uint64_t off; std::vector<uint8_t> data; };
  void die(uint64_t off, const void* buf, size_t len) {
    dead_ = true;
    if (cache_) {
      for (auto& w : pend_) {
        bool keep = (rng_() & 1) != 0;
        if (std::getenv("USBMEM_WRITE_LOG")) std::fprintf(stderr, "  crash: pending off=0x%llx len=%zu -> %s\n", (unsigned long long)w.off, w.data.size(), keep ? "persisted" : "lost");
        if (keep) p_->write_at(w.off, w.data.data(), w.data.size());
      }
      pend_.clear();
    }
    if (torn_ && len > 512) { size_t keep = (rng_() % (len / 512)) * 512; if (keep) p_->write_at(off, buf, keep); }
  }
  usbmem::BlockDevicePtr p_; long limit_, count_ = 0; bool cache_, torn_, dead_ = false; std::mt19937 rng_; std::vector<W> pend_; bool flush_cuts_ = std::getenv("USBMEM_CRASH_FLUSH_CUTS") != nullptr;
};
