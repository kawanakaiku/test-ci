// SPDX-License-Identifier: Apache-2.0
#include <fcntl.h>
#include <unistd.h>

#include <thread>

#include "test_util.h"
#include "usbmem/c_api.h"

using namespace usbmem;

static std::string temp_image(size_t bytes) {
  std::string tdir = test_tmpdir() + "/usbmem_test_XXXXXX";
  char* tmpl = tdir.data();
  int fd = mkstemp(tmpl);
  std::vector<uint8_t> d(bytes);
  for (size_t i = 0; i < bytes; i++) d[i] = (uint8_t)(i * 7 + 3);
  if (write(fd, d.data(), d.size()) != (ssize_t)d.size()) std::abort();
  close(fd);
  return std::string(tmpl);
}

TEST(raw_read_only_default) {
  std::string p = temp_image(8192);
  BlockDevicePtr d;
  CHECK(RawImageDevice::open_path(p, true, 512, &d).ok());
  CHECK_EQ(d->size(), 8192u);
  CHECK(d->is_read_only());
  uint8_t b[16];
  CHECK(d->read_at(100, b, 16).ok());
  CHECK_EQ(b[0], (uint8_t)(100 * 7 + 3));
  CHECK(d->write_at(0, b, 16).code == Errc::ReadOnly);
  CHECK(d->read_at(8192 - 8, b, 16).code == Errc::OutOfRange);
  CHECK(d->read_at(UINT64_MAX - 3, b, 16).code == Errc::OutOfRange);  // オーバーフロー
  CHECK(d->read_at(8192, b, 0).ok());                                 // 末尾ちょうどの 0 バイト
  CHECK(d->close().ok());
  CHECK(d->read_at(0, b, 1).code == Errc::Closed);
  CHECK(d->close().ok());  // 二重 close
  unlink(p.c_str());
}

TEST(raw_write_flush_persist) {
  std::string p = temp_image(4096);
  BlockDevicePtr d;
  CHECK(RawImageDevice::open_path(p, false, 512, &d).ok());
  const char msg[] = "hello";
  CHECK(d->write_at(4090, msg, 6).ok());
  CHECK(d->write_at(4091, msg, 6).code == Errc::OutOfRange);  // 拡張しない
  CHECK(d->flush().ok());
  d->close();
  BlockDevicePtr r;
  CHECK(RawImageDevice::open_path(p, true, 512, &r).ok());
  char out[6];
  CHECK(r->read_at(4090, out, 6).ok());
  CHECK(std::memcmp(out, msg, 6) == 0);
  CHECK_EQ(r->size(), 4096u);
  unlink(p.c_str());
}

TEST(raw_open_errors) {
  BlockDevicePtr d;
  CHECK(RawImageDevice::open_path("/nonexistent/x.img", true, 512, &d).code == Errc::Io);
  CHECK(RawImageDevice::open_fd(-1, true, false, 512, &d).code == Errc::InvalidArgument);
  CHECK(RawImageDevice::open_fd(0, true, false, 100, &d).code == Errc::InvalidArgument);  // 2 の冪でない
}

TEST(raw_cancel) {
  std::string p = temp_image(3 << 20);
  BlockDevicePtr d;
  CHECK(RawImageDevice::open_path(p, true, 512, &d).ok());
  CancelToken tok;
  tok.cancel();
  IoOptions o; o.cancel = &tok;
  std::vector<uint8_t> big(3 << 20);
  CHECK(d->read_at(0, big.data(), big.size(), o).code == Errc::Cancelled);
  unlink(p.c_str());
}

TEST(raw_large_read_matches) {
  std::string p = temp_image((2 << 20) + 123);  // チャンク境界をまたぐ
  BlockDevicePtr d;
  CHECK(RawImageDevice::open_path(p, true, 512, &d).ok());
  std::vector<uint8_t> big((2 << 20) + 123);
  CHECK(d->read_at(0, big.data(), big.size()).ok());
  bool same = true;
  for (size_t i = 0; i < big.size(); i++) same &= big[i] == (uint8_t)(i * 7 + 3);
  CHECK(same);
  unlink(p.c_str());
}

TEST(raw_concurrent_writes_serialized) {
  std::string p = temp_image(1 << 16);
  BlockDevicePtr d;
  CHECK(RawImageDevice::open_path(p, false, 512, &d).ok());
  std::vector<std::thread> th;
  for (int t = 0; t < 4; t++)
    th.emplace_back([&, t] {
      std::vector<uint8_t> buf(4096, (uint8_t)(t + 1));
      for (int i = 0; i < 50; i++) d->write_at(t * 4096, buf.data(), buf.size());
    });
  for (auto& x : th) x.join();
  for (int t = 0; t < 4; t++) {
    std::vector<uint8_t> buf(4096);
    CHECK(d->read_at(t * 4096, buf.data(), buf.size()).ok());
    CHECK(buf[0] == t + 1 && buf[4095] == t + 1);
  }
  unlink(p.c_str());
}

TEST(subrange_bounds_and_ro) {
  auto mem = std::make_shared<MemoryDevice>(std::vector<uint8_t>(4096, 0xAA), false);
  BlockDevicePtr sub;
  CHECK(SubRangeDevice::create(mem, 1024, 1024, false, &sub).ok());
  CHECK_EQ(sub->size(), 1024u);
  uint8_t v = 0x55;
  CHECK(sub->write_at(0, &v, 1).ok());
  CHECK_EQ(mem->bytes()[1024], 0x55);
  CHECK(sub->write_at(1024, &v, 1).code == Errc::OutOfRange);
  CHECK(sub->read_at(1023, &v, 2).code == Errc::OutOfRange);
  CHECK(SubRangeDevice::create(mem, 4000, 200, false, &sub).code == Errc::OutOfRange);
  CHECK(SubRangeDevice::create(mem, UINT64_MAX, 10, false, &sub).code == Errc::OutOfRange);
  BlockDevicePtr ro;
  CHECK(SubRangeDevice::create(mem, 0, 512, true, &ro).ok());
  CHECK(ro->write_at(0, &v, 1).code == Errc::ReadOnly);
}

TEST(c_api_roundtrip) {
  auto img = tb::make_gpt_disk(512, 4096, {{1, 2048, 4000, u"データ"}});
  std::string tdir = test_tmpdir() + "/usbmem_capi_XXXXXX";
  char* tmpl = tdir.data();
  int fd = mkstemp(tmpl);
  CHECK(write(fd, img.data(), img.size()) == (ssize_t)img.size());
  usbmem_device* h = nullptr;
  CHECK_EQ(usbmem_open_fd(fd, 1, 1, &h), 0);
  char* json = nullptr;
  CHECK_EQ(usbmem_list_partitions_json(h, &json), 0);
  CHECK(json && std::strstr(json, "\"kind\":\"gpt\""));
  CHECK(std::strstr(json, "データ"));
  usbmem_free(json);
  usbmem_close(h);
  CHECK(usbmem_open_fd(-1, 1, 0, &h) != 0);
  CHECK(usbmem_list_partitions_json(nullptr, &json) != 0);
  usbmem_close(nullptr);
  unlink(tmpl);
}

TEST(usb_msc_open_rejects_bad_arguments_and_non_usb_fd) {
  usbmem_device* d = nullptr;
  CHECK(usbmem_open_usb_msc(-1, 0x81, 0x02, 0, 0, 1, &d) != 0);
  CHECK(usbmem_open_usb_msc(0, 0x02, 0x81, 0, 0, 1, &d) != 0);   // IN/OUT 取り違え
  CHECK(usbmem_open_usb_msc(0, 0x81, 0x02, 0, 99, 1, &d) != 0);  // LUN 範囲外
  CHECK(d == nullptr);
  // USB ではない fd (通常ファイル) への ioctl は ENOTTY → 安全にエラーになる (クラッシュ・ハングしない)
  std::string p = temp_image(4096);
  int fd = ::open(p.c_str(), O_RDONLY);
  CHECK(fd >= 0);
  CHECK(usbmem_open_usb_msc(fd, 0x81, 0x02, 0, 0, 1, &d) != 0);
  CHECK(d == nullptr);
  ::close(fd);
  ::unlink(p.c_str());
}

// ---- ReadAheadDevice: 連続読みを先読みでまとめ、ランダム読みは増幅せず、書き込みで窓を捨てる
namespace {
class CountingMem final : public BlockDevice {
 public:
  explicit CountingMem(size_t n, uint32_t bs = 512) : d(n), bs_(bs) { for (size_t i = 0; i < n; i++) d[i] = (uint8_t)(i * 131 + (i >> 8) * 7); }
  std::vector<uint8_t> d; uint64_t reads = 0, bytes = 0; bool fail_reads = false; uint64_t fail_from = UINT64_MAX;
  Status read_at(uint64_t off, void* b, size_t len, const IoOptions& = {}) override {
    if (!range_ok(d.size(), off, len)) return Status::Make(Errc::OutOfRange);
    ++reads; bytes += len;
    if (fail_reads || (off + len > fail_from)) return Status::Make(Errc::Io);
    std::memcpy(b, d.data() + off, len); return Status::Ok();
  }
  Status write_at(uint64_t off, const void* b, size_t len, const IoOptions& = {}) override {
    if (!range_ok(d.size(), off, len)) return Status::Make(Errc::OutOfRange);
    std::memcpy(d.data() + off, b, len); return Status::Ok();
  }
  Status flush() override { return Status::Ok(); }
  Status close() override { return Status::Ok(); }
  uint64_t size() const override { return d.size(); }
  uint32_t logical_block_size() const override { return bs_; }
  bool is_read_only() const override { return false; }
  std::string describe() const override { return "countingmem"; }
 private: uint32_t bs_;
};
}  // namespace

TEST(readahead_sequential_reads_are_batched_and_correct) {
  auto mem = std::make_shared<CountingMem>(4u << 20);
  auto ra = ReadAheadDevice::wrap(mem, 256 * 1024);
  std::vector<uint8_t> b(4096);
  for (uint64_t off = 0; off < mem->d.size(); off += 4096) {
    CHECK(ra->read_at(off, b.data(), 4096).ok());
    CHECK(std::memcmp(b.data(), mem->d.data() + off, 4096) == 0);
  }
  // 1024 回の 4 KiB 読みが、16 回の窓 + 立ち上がりの数回に収まる
  CHECK(mem->reads <= 16 + 3);
  CHECK(mem->bytes <= (4u << 20) + 3 * 4096);   // 読み過ぎ (増幅) は立ち上がりの数ブロックだけ
}

TEST(readahead_random_reads_are_not_amplified) {
  auto mem = std::make_shared<CountingMem>(4u << 20);
  auto ra = ReadAheadDevice::wrap(mem, 256 * 1024);
  std::vector<uint8_t> b(4096);
  uint64_t x = 88172645463325252ull; int n = 300;
  for (int i = 0; i < n; i++) {
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    uint64_t off = (x % (mem->d.size() / 4096)) * 4096;
    CHECK(ra->read_at(off, b.data(), 4096).ok());
    CHECK(std::memcmp(b.data(), mem->d.data() + off, 4096) == 0);
  }
  CHECK(mem->bytes <= (uint64_t)n * 4096 * 2);   // ほぼ素通し (偶然の連続だけ先読みされる)
}

TEST(readahead_stats_count_device_reads) {
  auto mem = std::make_shared<CountingMem>(1u << 20);
  auto ra = ReadAheadDevice::wrap(mem, 256 * 1024);
  std::vector<uint8_t> b(4096);
  for (uint64_t off = 0; off < 64 * 4096; off += 4096) CHECK(ra->read_at(off, b.data(), 4096).ok());
  uint64_t reads = 0, bytes = 0, ns = 0;
  CHECK(ReadAheadDevice::stats(ra, &reads, &bytes, &ns));
  CHECK_EQ(reads, mem->reads);
  CHECK_EQ(bytes, mem->bytes);
  CHECK(!ReadAheadDevice::stats(mem, &reads, &bytes, &ns));   // ラップされていないデバイスは false
}

TEST(readahead_write_invalidates_window) {
  auto mem = std::make_shared<CountingMem>(1u << 20);
  auto ra = ReadAheadDevice::wrap(mem, 256 * 1024);
  std::vector<uint8_t> b(4096);
  for (uint64_t off = 0; off < 5 * 4096; off += 4096) CHECK(ra->read_at(off, b.data(), 4096).ok());   // 窓ができる
  std::vector<uint8_t> w(512, 0xEE);
  CHECK(ra->write_at(4096 * 10, w.data(), w.size()).ok());   // 窓の中への書き込み
  std::vector<uint8_t> r(4096);
  CHECK(ra->read_at(4096 * 10, r.data(), 4096).ok());
  CHECK(std::memcmp(r.data(), w.data(), 512) == 0);          // 古い内容ではなく、書いた内容が読める
  CHECK(std::memcmp(r.data() + 512, mem->d.data() + 4096 * 10 + 512, 3584) == 0);
}

TEST(readahead_tail_and_failure_semantics) {
  auto mem = std::make_shared<CountingMem>(300 * 1024 + 512);
  auto ra = ReadAheadDevice::wrap(mem, 256 * 1024);
  std::vector<uint8_t> b(4096);
  // 末尾の端数まで連続で読めること (窓がデバイス末尾を越えない)
  uint64_t off = 0;
  for (; off + 4096 <= mem->d.size(); off += 4096) { CHECK(ra->read_at(off, b.data(), 4096).ok()); CHECK(std::memcmp(b.data(), mem->d.data() + off, 4096) == 0); }
  std::vector<uint8_t> t(mem->d.size() - off);
  CHECK(ra->read_at(off, t.data(), t.size()).ok());
  CHECK(std::memcmp(t.data(), mem->d.data() + off, t.size()) == 0);
  CHECK(!ra->read_at(mem->d.size() - 100, b.data(), 4096).ok());   // 範囲外は OutOfRange
  // 先読みが失敗しても、要求した範囲だけで読めれば成功する (先読みのせいで読めたはずの所が失敗しない)
  auto mem2 = std::make_shared<CountingMem>(1u << 20);
  mem2->fail_from = 64 * 1024 + 4096;   // これより先を含む読み取りは失敗 (大きな窓は失敗し、4 KiB は成功する範囲)
  auto ra2 = ReadAheadDevice::wrap(mem2, 256 * 1024);
  for (uint64_t o = 0; o + 4096 <= 64 * 1024; o += 4096) CHECK(ra2->read_at(o, b.data(), 4096).ok());
  // 本当に読めない所はエラーのまま
  CHECK(!ra2->read_at(512 * 1024, b.data(), 4096).ok());
}
