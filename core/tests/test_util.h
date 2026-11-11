// SPDX-License-Identifier: Apache-2.0
// 最小のテストハーネスとディスクイメージビルダ(テスト専用)。
#pragma once
#include <cstdio>
#include <cstring>
#include <functional>
#include <string>
#include <vector>

#include "usbmem/crc32.h"
#include "usbmem/devices.h"
#include "usbmem/partition.h"

struct TestCase { const char* name; std::function<void()> fn; };
std::vector<TestCase>& registry();
extern int g_failures;
struct Registrar { Registrar(const char* n, std::function<void()> f) { registry().push_back({n, std::move(f)}); } };

#define TEST(name) \
  static void name(); \
  static Registrar reg_##name(#name, name); \
  static void name()
#define CHECK(cond) \
  do { if (!(cond)) { std::fprintf(stderr, "  CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #cond); g_failures++; } } while (0)
#define CHECK_EQ(a, b) \
  do { auto va_ = (a); auto vb_ = (b); if (!(va_ == vb_)) { std::fprintf(stderr, "  CHECK_EQ failed %s:%d: %s == %s (%llu vs %llu)\n", __FILE__, __LINE__, #a, #b, (unsigned long long)va_, (unsigned long long)vb_); g_failures++; } } while (0)

namespace tb {
using Bytes = std::vector<uint8_t>;
inline void le16(Bytes& b, size_t o, uint16_t v) { b[o] = v & 0xFF; b[o + 1] = v >> 8; }
inline void le32(Bytes& b, size_t o, uint32_t v) { for (int i = 0; i < 4; i++) b[o + i] = (v >> (8 * i)) & 0xFF; }
inline void le64(Bytes& b, size_t o, uint64_t v) { for (int i = 0; i < 8; i++) b[o + i] = (v >> (8 * i)) & 0xFF; }

struct MbrEnt { uint8_t status; uint8_t type; uint32_t start; uint32_t count; };
inline void put_mbr_entry(Bytes& sec, int slot, const MbrEnt& e) {
  size_t o = 446 + 16 * slot;
  sec[o] = e.status; sec[o + 4] = e.type; le32(sec, o + 8, e.start); le32(sec, o + 12, e.count);
}
inline void sign55aa(Bytes& sec) { sec[510] = 0x55; sec[511] = 0xAA; }

struct GptEnt { uint8_t type_first_byte; uint64_t first, last; std::u16string name; };

// ss バイトセクタ, total_sectors の GPT ディスクを作る。
inline Bytes make_gpt_disk(uint32_t ss, uint64_t total_sectors, const std::vector<GptEnt>& ents,
                           uint32_t num_entries = 128) {
  Bytes img(total_sectors * ss, 0);
  // 保護 MBR
  Bytes mbr(ss, 0);
  put_mbr_entry(mbr, 0, {0, 0xEE, 1, (uint32_t)(total_sectors - 1)});
  sign55aa(mbr);
  std::memcpy(img.data(), mbr.data(), ss);

  Bytes arr(num_entries * 128, 0);
  for (size_t i = 0; i < ents.size(); i++) {
    size_t o = i * 128;
    arr[o] = ents[i].type_first_byte;  // 非ゼロなら使用中
    arr[o + 1] = 0xAB;
    arr[o + 16] = (uint8_t)(i + 1);
    le64(arr, o + 32, ents[i].first);
    le64(arr, o + 40, ents[i].last);
    for (size_t k = 0; k < ents[i].name.size() && k < 36; k++) le16(arr, o + 56 + 2 * k, ents[i].name[k]);
  }
  uint64_t arr_secs = (arr.size() + ss - 1) / ss;
  uint64_t first_usable = 2 + arr_secs, last_usable = total_sectors - 2 - arr_secs;
  auto header = [&](uint64_t my, uint64_t alt, uint64_t ent_lba) {
    Bytes h(ss, 0);
    std::memcpy(h.data(), "EFI PART", 8);
    le32(h, 8, 0x00010000); le32(h, 12, 92);
    le64(h, 24, my); le64(h, 32, alt); le64(h, 40, first_usable); le64(h, 48, last_usable);
    for (int i = 0; i < 16; i++) h[56 + i] = (uint8_t)(0x10 + i);
    le64(h, 72, ent_lba); le32(h, 80, num_entries); le32(h, 84, 128);
    le32(h, 88, usbmem::crc32(arr.data(), arr.size()));
    le32(h, 16, usbmem::crc32(h.data(), 92));
    return h;
  };
  Bytes ph = header(1, total_sectors - 1, 2);
  Bytes bh = header(total_sectors - 1, 1, total_sectors - 1 - arr_secs);
  std::memcpy(img.data() + ss, ph.data(), ss);
  std::memcpy(img.data() + 2 * ss, arr.data(), arr.size());
  std::memcpy(img.data() + (total_sectors - 1 - arr_secs) * ss, arr.data(), arr.size());
  std::memcpy(img.data() + (total_sectors - 1) * ss, bh.data(), ss);
  return img;
}

// 指定回数後に失敗 / 短い read を返す(実デバイスの異常再現)。
class FaultDevice final : public usbmem::BlockDevice {
 public:
  FaultDevice(usbmem::BlockDevicePtr inner) : in_(std::move(inner)) {}
  int fail_after_reads = -1;  // N 回成功後に Io
  usbmem::Errc fail_code = usbmem::Errc::Io;
  int reads = 0;
  usbmem::Status read_at(uint64_t o, void* b, size_t n, const usbmem::IoOptions& io = {}) override {
    if (fail_after_reads >= 0 && reads++ >= fail_after_reads) return usbmem::Status::Make(fail_code, 5);
    return in_->read_at(o, b, n, io);
  }
  usbmem::Status write_at(uint64_t o, const void* b, size_t n, const usbmem::IoOptions& io = {}) override { return in_->write_at(o, b, n, io); }
  usbmem::Status flush() override { return in_->flush(); }
  usbmem::Status close() override { return in_->close(); }
  uint64_t size() const override { return in_->size(); }
  uint32_t logical_block_size() const override { return in_->logical_block_size(); }
  bool is_read_only() const override { return in_->is_read_only(); }
  std::string describe() const override { return "fault"; }
 private:
  usbmem::BlockDevicePtr in_;
};

inline bool has_diag(const usbmem::PartitionTable& t, const char* code) {
  for (auto& d : t.diagnostics) if (d.code == code) return true;
  return false;
}
inline usbmem::Status parse(Bytes img, usbmem::PartitionTable* t, uint32_t bs = 512) {
  usbmem::MemoryDevice dev(std::move(img), true, bs);
  return usbmem::read_partition_table(dev, t);
}
}  // namespace tb
