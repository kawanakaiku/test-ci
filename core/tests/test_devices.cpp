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
