// SPDX-License-Identifier: Apache-2.0
// パーティション編集テスト用の部品: 書き込み障害 / 電源断を再現するデバイス、疎な巨大デバイス、表のスナップショット。
#pragma once
#include <map>
#include <random>
#include <set>
#include <tuple>
#include <unordered_map>

#include "test_util.h"
#include "usbmem/partition_edit.h"

namespace pt {
using namespace usbmem;
using tb::Bytes;

// MemoryDevice を包み、書き込みを記録し、指定した書き込みで失敗 / 電源断を起こす。
class WDev final : public BlockDevice {
 public:
  explicit WDev(Bytes img, uint32_t bs = 512) : mem_(std::make_shared<MemoryDevice>(std::move(img), false, bs)) {}
  std::shared_ptr<MemoryDevice> mem_;
  std::vector<std::pair<uint64_t, size_t>> writes;  // (offset, len) 成功した書き込み
  std::vector<int> flush_at;                        // flush 時点での書き込み数
  int fail_write_index = -1;  // この番号 (0 始まり) の書き込みで失敗
  bool torn = false;          // 失敗する書き込みの前半だけを反映する
  int fail_flush_index = -1;
  int cut_after = -1;         // 成功した書き込みがこの数に達したら、以降の書き込みは一切反映しない (電源断)
  bool cut_torn = false;      // 電源断の瞬間の書き込みを半分だけ反映する
  int attempts = 0;
  bool locked = false;        // true なら write_at は ReadOnly を返す

  Status read_at(uint64_t o, void* b, size_t n, const IoOptions& io = {}) override { return mem_->read_at(o, b, n, io); }
  Status write_at(uint64_t o, const void* b, size_t n, const IoOptions& io = {}) override {
    int idx = attempts++;
    if (locked) return Status::Make(Errc::ReadOnly);
    if (cut_after >= 0 && static_cast<int>(writes.size()) >= cut_after) {
      if (cut_torn && static_cast<int>(writes.size()) == cut_after && !cut_done_) {
        cut_done_ = true;
        mem_->write_at(o, b, n / 2, io);
      }
      return Status::Make(Errc::Io, 5);
    }
    if (fail_write_index >= 0 && idx == fail_write_index) {
      if (torn) mem_->write_at(o, b, n / 2, io);
      return Status::Make(Errc::Io, 5);
    }
    Status st = mem_->write_at(o, b, n, io);
    if (st.ok()) writes.push_back({o, n});
    return st;
  }
  Status flush() override {
    int idx = static_cast<int>(flush_at.size());
    flush_at.push_back(static_cast<int>(writes.size()));
    if (fail_flush_index >= 0 && idx == fail_flush_index) return Status::Make(Errc::Io, 5);
    return Status::Ok();
  }
  Status close() override { return Status::Ok(); }
  uint64_t size() const override { return mem_->size(); }
  uint32_t logical_block_size() const override { return mem_->logical_block_size(); }
  bool is_read_only() const override { return false; }
  std::string describe() const override { return "wdev"; }
  Bytes& bytes() { return mem_->bytes(); }

 private:
  bool cut_done_ = false;
};

// 大きさだけ巨大なデバイス (書かれたセクタだけ保持する)。2 TiB 超のディスクの検証用。
class SparseDevice final : public BlockDevice {
 public:
  SparseDevice(uint64_t bytes, uint32_t ss = 512) : size_(bytes), ss_(ss) {}
  Status read_at(uint64_t o, void* b, size_t n, const IoOptions& = {}) override {
    if (!range_ok(size_, o, n)) return Status::Make(Errc::OutOfRange);
    uint8_t* p = static_cast<uint8_t*>(b);
    std::memset(p, 0, n);
    for (uint64_t s = o / ss_; s * ss_ < o + n; s++) {
      auto it = secs_.find(s);
      if (it == secs_.end()) continue;
      uint64_t lo = std::max<uint64_t>(s * ss_, o), hi = std::min<uint64_t>((s + 1) * ss_, o + n);
      std::memcpy(p + (lo - o), it->second.data() + (lo - s * ss_), hi - lo);
    }
    return Status::Ok();
  }
  Status write_at(uint64_t o, const void* b, size_t n, const IoOptions& = {}) override {
    if (!range_ok(size_, o, n)) return Status::Make(Errc::OutOfRange);
    const uint8_t* p = static_cast<const uint8_t*>(b);
    for (uint64_t s = o / ss_; s * ss_ < o + n; s++) {
      auto& v = secs_[s];
      if (v.empty()) v.assign(ss_, 0);
      uint64_t lo = std::max<uint64_t>(s * ss_, o), hi = std::min<uint64_t>((s + 1) * ss_, o + n);
      std::memcpy(v.data() + (lo - s * ss_), p + (lo - o), hi - lo);
    }
    return Status::Ok();
  }
  Status flush() override { return Status::Ok(); }
  Status close() override { return Status::Ok(); }
  uint64_t size() const override { return size_; }
  uint32_t logical_block_size() const override { return ss_; }
  bool is_read_only() const override { return false; }
  std::string describe() const override { return "sparse"; }
  size_t sectors_held() const { return secs_.size(); }

 private:
  uint64_t size_;
  uint32_t ss_;
  std::unordered_map<uint64_t, Bytes> secs_;
};

struct Snap {
  TableKind kind = TableKind::None;
  std::string id;
  std::vector<std::tuple<uint64_t, uint64_t, uint32_t, std::string, std::string, uint64_t, std::string>> parts;
  bool gpt_usable = false;  // GPT のとき、プライマリかバックアップのどちらかが完全に有効
  bool operator==(const Snap& o) const { return kind == o.kind && id == o.id && parts == o.parts; }
  bool operator!=(const Snap& o) const { return !(*this == o); }
};

inline Snap snap(BlockDevice& dev, uint32_t ss = 512) {
  Snap s;
  PartitionTable t;
  ParseOptions po;
  po.sector_size = ss;
  po.probe_filesystems = false;
  po.read_labels = false;
  if (!read_partition_table(dev, &t, po).ok()) return s;
  s.kind = t.kind;
  s.id = t.kind == TableKind::Gpt ? t.disk_guid : t.mbr_disk_id;
  for (auto& p : t.partitions)
    s.parts.push_back({p.start_lba, p.sector_count, p.mbr_type, guid_to_string(p.type_guid), guid_to_string(p.unique_guid), p.gpt_attributes, p.name});
  if (t.kind == TableKind::Gpt) {
    GptInspection gi;
    inspect_gpt(dev, ss, &gi);
    s.gpt_usable = gi.primary.usable() || gi.backup.usable();
  }
  return s;
}

inline Bytes read_all(BlockDevice& d) {
  Bytes b(d.size());
  d.read_at(0, b.data(), b.size());
  return b;
}

inline std::array<uint8_t, 16> guid_of(const char* s) {
  std::array<uint8_t, 16> g{};
  parse_guid(s, &g);
  return g;
}

inline bool all_zero_range(const Bytes& b, size_t off, size_t n) {
  for (size_t i = 0; i < n; i++)
    if (b[off + i]) return false;
  return true;
}

inline Bytes blank(uint64_t sectors, uint32_t ss = 512) { return Bytes(sectors * ss, 0); }

// MBR ディスク: 主 3 つ + 拡張 (論理 2 つ)
inline Bytes mbr_disk(uint64_t sectors, uint32_t ss = 512) {
  Bytes img = blank(sectors, ss);
  tb::put_mbr_entry(img, 0, {0x80, 0x83, 2048, 4096});
  tb::put_mbr_entry(img, 1, {0, 0x0C, 8192, 4096});
  tb::put_mbr_entry(img, 2, {0, 0x05, 16384, 8192});
  tb::le32(img, 440, 0xC0FFEE01);
  tb::sign55aa(img);
  // EBR 1: 先頭 (16384) → 論理 (16384+2048, 2048) 次の EBR は 16384+4096
  Bytes e1(ss, 0);
  tb::put_mbr_entry(e1, 0, {0, 0x83, 2048, 2048});
  tb::put_mbr_entry(e1, 1, {0, 0x05, 4096, 2048 + 1});
  tb::sign55aa(e1);
  std::memcpy(img.data() + 16384ull * ss, e1.data(), ss);
  Bytes e2(ss, 0);
  tb::put_mbr_entry(e2, 0, {0, 0x83, 1, 2048});
  tb::sign55aa(e2);
  std::memcpy(img.data() + (16384ull + 4096) * ss, e2.data(), ss);
  return img;
}

inline Bytes gpt_disk3(uint64_t sectors, uint32_t ss = 512) {
  return tb::make_gpt_disk(ss, sectors, {{0x28, 2048, 4095, u"one"}, {0x28, 8192, 12287, u"two"}, {0x28, 16384, 20479, u"three"}});
}

}  // namespace pt
