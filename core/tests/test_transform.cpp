// SPDX-License-Identifier: Apache-2.0
// BlockTransform 層の単体テスト: 復号デバイスの共通動作 (キャッシュ・範囲・書き込み拒否・close でゼロ化)、
// JSON / base64 の上限、暗号ボリュームでないデバイスの扱い、C API の引数検査。暗号方式ごとの正しさは test_crypto.cpp と
// compat/tests/test_cryptovol.cpp (実フィクスチャ) が受け持つ。
#include <atomic>
#include <random>
#include <thread>

#include "mini_json.h"
#include "test_util.h"
#include "usbmem/c_api_internal.h"
#include "usbmem/c_api_transform.h"
#include "usbmem/transform.h"

using namespace usbmem;

namespace {

uint8_t plain_byte(uint64_t i) { return uint8_t((i * 131 + (i >> 9) * 17 + 5) & 0xff); }

// 疑似復号: 平文 = plain_byte(位置)。暗号文は使わず、キャッシュ・範囲検査・ライフサイクルだけを検証する。
class FakeDevice final : public DecryptingDevice {
 public:
  FakeDevice(BlockDevicePtr base, uint64_t size, uint32_t cache, std::atomic<int>* wiped, std::atomic<uint64_t>* reads)
      : DecryptingDevice(base, size, 512, cache, "fake"), wiped_(wiped), reads_(reads) {}

 protected:
  Status read_plain(uint64_t off, uint8_t* buf, size_t len, const IoOptions&) override {
    if (off % 512 || len % 512 || len == 0) { bad_align_ = true; return Status::Make(Errc::InvalidArgument); }
    reads_->fetch_add(len);
    for (size_t i = 0; i < len; i++) buf[i] = plain_byte(off + i);
    return Status::Ok();
  }
  void wipe_keys() override { wiped_->fetch_add(1); }

 public:
  bool bad_align_ = false;

 private:
  std::atomic<int>* wiped_;
  std::atomic<uint64_t>* reads_;
};

}  // namespace

TEST(transform_decrypting_device_basics) {
  auto base = std::make_shared<MemoryDevice>(std::vector<uint8_t>(4096, 0));
  std::atomic<int> wiped{0};
  std::atomic<uint64_t> reads{0};
  const uint64_t size = 1024 * 1024 + 512 * 3;
  auto dev = std::make_shared<FakeDevice>(base, size, 4, &wiped, &reads);
  CHECK_EQ(dev->size(), size);
  CHECK(dev->is_read_only());
  // 任意の (非整列を含む) 範囲読み出しが参照と一致する
  std::mt19937_64 rng(7);
  std::vector<uint8_t> buf;
  for (int it = 0; it < 2000; it++) {
    uint64_t off = rng() % size;
    size_t len = size_t(rng() % 70000) + 1;
    if (off + len > size) len = size_t(size - off);
    buf.assign(len, 0xee);
    Status st = dev->read_at(off, buf.data(), len);
    CHECK(st.ok());
    bool same = true;
    for (size_t i = 0; i < len && same; i++) same = buf[i] == plain_byte(off + i);
    CHECK(same);
  }
  CHECK(!dev->bad_align_);
  // 範囲外・オーバーフローは OutOfRange (確保も読み出しもしない)
  uint8_t b[16];
  CHECK(dev->read_at(size - 8, b, 16).code == Errc::OutOfRange);
  CHECK(dev->read_at(size, b, 1).code == Errc::OutOfRange);
  CHECK(dev->read_at(~uint64_t(0) - 4, b, 16).code == Errc::OutOfRange);
  CHECK(dev->read_at(0, b, 0).ok());
  // 書き込みは常に ReadOnly
  CHECK(dev->write_at(0, b, 16).code == Errc::ReadOnly);
  CHECK(dev->flush().ok());
  // close: 鍵のゼロ化は 1 度だけ。以後の読み出しは Closed。親は閉じない
  CHECK_EQ(wiped.load(), 0);
  CHECK(dev->close().ok());
  CHECK(dev->close().ok());
  CHECK_EQ(wiped.load(), 1);
  CHECK(dev->read_at(0, b, 16).code == Errc::Closed);
  uint8_t pb[4];
  CHECK(base->read_at(0, pb, 4).ok());  // 親は生きている
  dev.reset();
  CHECK_EQ(wiped.load(), 1);
}

TEST(transform_decrypting_device_threads) {
  auto base = std::make_shared<MemoryDevice>(std::vector<uint8_t>(4096, 0));
  std::atomic<int> wiped{0};
  std::atomic<uint64_t> reads{0};
  const uint64_t size = 2 * 1024 * 1024;
  auto dev = std::make_shared<FakeDevice>(base, size, 3, &wiped, &reads);
  std::atomic<int> bad{0};
  std::vector<std::thread> th;
  for (int t = 0; t < 4; t++)
    th.emplace_back([&, t] {
      std::mt19937_64 rng(100 + t);
      std::vector<uint8_t> b;
      for (int it = 0; it < 400; it++) {
        uint64_t off = rng() % (size - 1);
        size_t len = std::min<uint64_t>(size - off, size_t(rng() % 40000) + 1);
        b.assign(len, 0);
        if (!dev->read_at(off, b.data(), len).ok()) { bad++; continue; }
        for (size_t i = 0; i < len; i++)
          if (b[i] != plain_byte(off + i)) { bad++; break; }
      }
    });
  for (auto& x : th) x.join();
  CHECK_EQ(bad.load(), 0);
  // 読み出し中の close でも落ちず、以後は Closed
  std::thread rd([&] {
    std::vector<uint8_t> b(65536);
    for (int i = 0; i < 200; i++) dev->read_at(uint64_t(i) * 4096, b.data(), b.size());
  });
  dev->close();
  rd.join();
  CHECK_EQ(wiped.load(), 1);
}

TEST(transform_json_limits) {
  using namespace usbmem::tf;
  JsonValue v;
  std::string err;
  std::string ok = R"({"a":{"b":[1,2,"x"]},"n":"18446744073709551615","m":"-1"})";
  CHECK(json_parse(ok.data(), ok.size(), &v, &err));
  uint64_t u = 0;
  CHECK(v.get("n") && v.get("n")->as_u64(&u) && u == ~uint64_t(0));
  CHECK(v.get("m") && !v.get("m")->as_u64(&u));
  // 深いネストは拒否 (再帰で溢れない)
  std::string deep(100000, '[');
  CHECK(!json_parse(deep.data(), deep.size(), &v, &err));
  std::string deep2 = std::string(100000, '[') + std::string(100000, ']');
  CHECK(!json_parse(deep2.data(), deep2.size(), &v, &err));
  // 巨大な配列 (ノード数上限)
  std::string big = "[";
  for (int i = 0; i < 300000; i++) big += "1,";
  big += "1]";
  CHECK(!json_parse(big.data(), big.size(), &v, &err));
  // 壊れた入力
  for (const char* s : {"", "{", "{\"a\":}", "[1,]", "\"abc", "{\"a\" 1}", "nul", "[1 2]", "{\"a\":1}x", "\"\\u12\""}) CHECK(!json_parse(s, strlen(s), &v, &err));
  // base64
  std::vector<uint8_t> out;
  CHECK(base64_decode("aGVsbG8=", &out, 100) && out.size() == 5 && out[0] == 'h');
  CHECK(base64_decode("aGVsbG8", &out, 100) && out.size() == 5);
  CHECK(!base64_decode("aGVs!G8=", &out, 100));
  CHECK(!base64_decode("aGVsbG8=", &out, 4));  // 上限超え
}

TEST(transform_not_a_crypto_volume) {
  // ゼロ埋め・乱数・極小デバイスは暗号ボリュームではない (Unsupported)。クラッシュしない。
  std::mt19937_64 rng(3);
  for (size_t sz : {size_t(0), size_t(1), size_t(100), size_t(512), size_t(4096), size_t(1 << 20)}) {
    for (int kind = 0; kind < 2; kind++) {
      std::vector<uint8_t> d(sz);
      if (kind) for (auto& x : d) x = uint8_t(rng());
      auto dev = std::make_shared<MemoryDevice>(std::move(d));
      VolumeInfo info;
      Status st = probe_crypto_volume(*dev, &info);
      CHECK(!st.ok());
      CHECK(info.kind == CryptoKind::Auto);
      BlockDevicePtr out;
      Credentials c;
      c.password.assign(reinterpret_cast<const uint8_t*>("x"), 1);
      st = open_crypto_volume(dev, CryptoKind::Auto, c, OpenOptions{}, &out);
      CHECK(!st.ok());
      CHECK(!out);
      st = open_crypto_volume(dev, CryptoKind::BitLocker, c, OpenOptions{}, &out);
      CHECK(!st.ok());
      st = open_crypto_volume(dev, CryptoKind::Luks, c, OpenOptions{}, &out);
      CHECK(!st.ok());
    }
  }
}

TEST(transform_c_api_arguments) {
  auto* h = new usbmem_device{std::make_shared<MemoryDevice>(std::vector<uint8_t>(8192, 0))};
  char* json = nullptr;
  CHECK_EQ(usbmem_transform_probe(nullptr, &json), int(Errc::InvalidArgument));
  CHECK_EQ(usbmem_transform_probe(h, nullptr), int(Errc::InvalidArgument));
  CHECK_EQ(usbmem_transform_probe(h, &json), int(Errc::Unsupported));
  CHECK(json == nullptr);
  usbmem_device* out = nullptr;
  unsigned char pw[4] = {'a', 'b', 'c', 'd'};
  CHECK_EQ(usbmem_transform_open(nullptr, 0, pw, 4, nullptr, 0, nullptr, 0, 0, &out, nullptr), int(Errc::InvalidArgument));
  CHECK_EQ(usbmem_transform_open(h, 9, pw, 4, nullptr, 0, nullptr, 0, 0, &out, nullptr), int(Errc::InvalidArgument));
  CHECK_EQ(usbmem_transform_open(h, 0, nullptr, 4, nullptr, 0, nullptr, 0, 0, &out, nullptr), int(Errc::InvalidArgument));
  CHECK_EQ(usbmem_transform_open(h, 0, pw, 1u << 20, nullptr, 0, nullptr, 0, 0, &out, nullptr), int(Errc::InvalidArgument));
  CHECK(usbmem_transform_open(h, 0, pw, 4, nullptr, 0, nullptr, 0, 0, &out, nullptr) != 0);
  CHECK(out == nullptr);
  usbmem_close(h);
}

// 小さい読みの扱い: ランダムな 4 KiB 読みは 4 KiB ずつしか下位から読まない (先読みで膨らまない)、連続読みは先読みで下位の読み回数が減る、
// キャッシュに載っているデータの再読みは下位に触れない。内容は常に参照と一致する。
namespace {
class CountingFake final : public DecryptingDevice {
 public:
  CountingFake(uint64_t size, uint32_t sector) : DecryptingDevice(std::make_shared<MemoryDevice>(std::vector<uint8_t>(4096, 0)), size, sector, 0, "cf") {}
  uint64_t calls = 0, bytes = 0;

 protected:
  Status read_plain(uint64_t off, uint8_t* buf, size_t len, const IoOptions&) override {
    calls++; bytes += len;
    for (size_t i = 0; i < len; i++) buf[i] = plain_byte(off + i);
    return Status::Ok();
  }
  void wipe_keys() override {}
};
bool matches(BlockDevice& d, uint64_t off, size_t len) {
  std::vector<uint8_t> b(len);
  if (!d.read_at(off, b.data(), len).ok()) return false;
  for (size_t i = 0; i < len; i++) if (b[i] != plain_byte(off + i)) return false;
  return true;
}
}  // namespace

TEST(transform_decrypting_device_readahead) {
  const uint64_t size = 64ull << 20;
  {  // ランダム 4 KiB: 1 回 4 KiB だけ
    CountingFake d(size, 512);
    std::mt19937_64 rng(3);
    for (int i = 0; i < 300; i++) CHECK(matches(d, (rng() % (size / 4096)) * 4096, 4096));
    CHECK(d.calls <= 300);
    CHECK(d.bytes <= 300ull * 4096);
  }
  {  // 連続 4 KiB: 先読みで下位の読みが大幅に減り、全体を読んでも読み過ぎない (末尾の先読み分だけ)
    CountingFake d(size, 512);
    for (uint64_t o = 0; o < (4u << 20); o += 4096) CHECK(matches(d, o, 4096));
    CHECK(d.calls < 100);
    CHECK(d.bytes <= (4u << 20) + 512 * 1024);
    uint64_t c = d.calls;
    for (uint64_t o = (3u << 20); o < (4u << 20); o += 4096) CHECK(matches(d, o, 4096));  // 直近の 1 MiB はキャッシュ内
    CHECK_EQ(d.calls, c);
  }
  {  // 非整列・ページをまたぐ・デバイス末尾の端数 (セクタ 4096 でも)
    for (uint32_t sec : {512u, 4096u}) {
      CountingFake d(size + sec * 3, sec);
      std::mt19937_64 rng(sec);
      for (int i = 0; i < 1500; i++) {
        uint64_t off = rng() % (d.size() - 1);
        size_t len = std::min<uint64_t>(d.size() - off, size_t(rng() % 20000) + 1);
        CHECK(matches(d, off, len));
      }
      CHECK(matches(d, d.size() - 100, 100));
      CHECK(matches(d, d.size() - 4096, 4096));
    }
  }
}
