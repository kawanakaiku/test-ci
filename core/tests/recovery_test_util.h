// SPDX-License-Identifier: Apache-2.0
// データ救出 / バックアップのテスト用部品: 不良セクタを再現するデバイス、書き込みを数えるデバイス、乱数。
#pragma once
#include <atomic>
#include <chrono>
#include <cstring>
#include <functional>
#include <map>
#include <mutex>
#include <thread>
#include <random>
#include <set>

#include "test_util.h"
#include "usbmem/backup.h"
#include "usbmem/devices.h"
#include "usbmem/recovery.h"

namespace rt {
using namespace usbmem;
using tb::Bytes;

// 内側のデバイスを包み、「範囲が不良セクタに少しでも重なる読み取りは全体が失敗する」現実のディスクの振る舞いを再現する。
class FaultDevice final : public BlockDevice {
 public:
  FaultDevice(BlockDevicePtr inner, uint32_t sector = 512) : in_(std::move(inner)), sector_(sector) {}
  std::set<uint64_t> bad;                      // 不良セクタ番号
  std::map<uint64_t, int> flaky;               // セクタ番号 → 成功するまでに失敗する回数
  uint64_t reads = 0, errors = 0, writes = 0;
  int64_t disconnect_after = -1;               // この回数の読み取りのあと Disconnected を返す
  uint64_t* fake_clock = nullptr;              // 読み取りごとに進める
  uint64_t ms_per_read = 0, ms_per_error = 0;
  std::function<void(uint64_t read_index)> on_read;  // 読み取りの直前に呼ぶ (取り消しを仕込む用)

  Status read_at(uint64_t off, void* buf, size_t len, const IoOptions& io = {}) override {
    uint64_t idx;
    {
      std::lock_guard<std::mutex> l(mu_);
      idx = reads++;
    }
    if (on_read) on_read(idx);
    if (fake_clock) *fake_clock += ms_per_read;
    if (disconnect_after >= 0 && static_cast<int64_t>(idx) >= disconnect_after) return Status::Make(Errc::Disconnected);
    bool fail = false;
    for (uint64_t s = off / sector_; s * sector_ < off + len; s++) {
      if (bad.count(s)) fail = true;
      auto it = flaky.find(s);
      if (it != flaky.end() && it->second > 0) { it->second--; fail = true; }
    }
    if (fail) {
      errors++;
      if (fake_clock) *fake_clock += ms_per_error;
      return Status::Make(Errc::Io, 5);
    }
    return in_->read_at(off, buf, len, io);
  }
  Status write_at(uint64_t, const void*, size_t, const IoOptions& = {}) override { writes++; return Status::Make(Errc::ReadOnly); }
  Status flush() override { return Status::Ok(); }
  Status close() override { return Status::Ok(); }
  uint64_t size() const override { return in_->size(); }
  uint32_t logical_block_size() const override { return sector_; }
  bool is_read_only() const override { return true; }
  std::string describe() const override { return "fault"; }

 private:
  BlockDevicePtr in_;
  uint32_t sector_;
  std::mutex mu_;
};

// 書き込みが失敗する出力
class FailingOut final : public BlockDevice {
 public:
  FailingOut(uint64_t size, int fail_at) : mem_(std::make_shared<MemoryDevice>(Bytes(size, 0), false, 512)), fail_at_(fail_at) {}
  std::shared_ptr<MemoryDevice> mem_;
  int fail_at_, n = 0;
  Status read_at(uint64_t o, void* b, size_t l, const IoOptions& io = {}) override { return mem_->read_at(o, b, l, io); }
  Status write_at(uint64_t o, const void* b, size_t l, const IoOptions& io = {}) override {
    if (n++ == fail_at_) return Status::Make(Errc::Io, 28);
    return mem_->write_at(o, b, l, io);
  }
  Status flush() override { return Status::Ok(); }
  Status close() override { return Status::Ok(); }
  uint64_t size() const override { return mem_->size(); }
  uint32_t logical_block_size() const override { return 512; }
  bool is_read_only() const override { return false; }
  std::string describe() const override { return "failing-out"; }
};

// 書き込みの回数と位置を記録し、指定した書き込みで電源断 (以降の書き込みを無視) を起こす
class RecordingDev final : public BlockDevice {
 public:
  explicit RecordingDev(Bytes img, uint32_t bs = 512) : mem_(std::make_shared<MemoryDevice>(std::move(img), false, bs)) {}
  std::shared_ptr<MemoryDevice> mem_;
  std::vector<std::pair<uint64_t, size_t>> writes;
  int cut_after = -1;
  int flushes = 0;
  bool ro = false;
  Status read_at(uint64_t o, void* b, size_t l, const IoOptions& io = {}) override { return mem_->read_at(o, b, l, io); }
  Status write_at(uint64_t o, const void* b, size_t l, const IoOptions& io = {}) override {
    if (ro) return Status::Make(Errc::ReadOnly);
    if (cut_after >= 0 && static_cast<int>(writes.size()) >= cut_after) return Status::Make(Errc::Io, 5);
    writes.push_back({o, l});
    return mem_->write_at(o, b, l, io);
  }
  Status flush() override { flushes++; return Status::Ok(); }
  Status close() override { return Status::Ok(); }
  uint64_t size() const override { return mem_->size(); }
  uint32_t logical_block_size() const override { return 512; }
  bool is_read_only() const override { return ro; }
  std::string describe() const override { return "recording"; }
  Bytes& bytes() { return mem_->bytes(); }
};

inline Bytes random_bytes(size_t n, uint32_t seed) {
  std::mt19937 g(seed);
  Bytes b(n);
  for (auto& x : b) x = static_cast<uint8_t>(g());
  return b;
}

// 小さな FAT16 を作る (mkfs なしでテストするため)。used_every 個おきのクラスタを使用中にして乱数データを入れる。
inline Bytes make_fat16(uint32_t total_sectors, uint32_t spc, uint32_t used_every, uint32_t seed, uint32_t bps = 512) {
  Bytes img(static_cast<size_t>(total_sectors) * bps, 0);
  uint32_t rsv = 1, nfats = 2, root_ent = 512, root_secs = root_ent * 32 / bps;
  uint32_t clusters_guess = total_sectors / spc;
  uint32_t fatsz = (clusters_guess + 2) * 2 / bps + 1;
  uint32_t first_data = rsv + nfats * fatsz + root_secs;
  uint32_t clusters = (total_sectors - first_data) / spc;
  tb::le16(img, 11, static_cast<uint16_t>(bps));
  img[13] = static_cast<uint8_t>(spc);
  tb::le16(img, 14, static_cast<uint16_t>(rsv));
  img[16] = static_cast<uint8_t>(nfats);
  tb::le16(img, 17, static_cast<uint16_t>(root_ent));
  if (total_sectors < 65536) tb::le16(img, 19, static_cast<uint16_t>(total_sectors)); else tb::le32(img, 32, total_sectors);
  img[21] = 0xF8;
  tb::le16(img, 22, static_cast<uint16_t>(fatsz));
  tb::le16(img, 24, 32); tb::le16(img, 26, 64);
  img[38] = 0x29;
  std::memcpy(&img[54], "FAT16   ", 8);
  img[0] = 0xEB; img[1] = 0x3C; img[2] = 0x90;
  std::memcpy(&img[3], "MSDOS5.0", 8);
  img[510] = 0x55; img[511] = 0xAA;
  std::mt19937 g(seed);
  for (int f = 0; f < 2; f++) { size_t o = (rsv + f * fatsz) * bps; tb::le16(img, o, 0xFFF8); tb::le16(img, o + 2, 0xFFFF); }
  for (uint32_t c = 2; c < clusters + 2; c++) {
    if ((c - 2) % used_every) continue;
    for (int f = 0; f < 2; f++) tb::le16(img, (rsv + f * fatsz) * bps + c * 2, 0xFFFF);
    size_t o = static_cast<size_t>(first_data + (c - 2) * spc) * bps;
    for (size_t i = 0; i < static_cast<size_t>(spc) * bps; i++) img[o + i] = static_cast<uint8_t>(g());
  }
  // ブート領域と FAT 領域にも乱数ではない目印を入れる
  for (size_t i = 62; i < 400; i++) img[i] = static_cast<uint8_t>(i * 7);
  return img;
}

}  // namespace rt
