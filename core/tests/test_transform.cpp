// SPDX-License-Identifier: Apache-2.0
// BlockTransform 層の単体テスト: 復号デバイスの共通動作 (キャッシュ・範囲・書き込み拒否・close でゼロ化)、
// JSON / base64 の上限、暗号ボリュームでないデバイスの扱い、C API の引数検査。暗号方式ごとの正しさは test_crypto.cpp と
// compat/tests/test_cryptovol.cpp (実フィクスチャ) が受け持つ。
#include <atomic>
#include <cstring>
#include <mutex>
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


// ---------------------------------------------------------------------------------------------------------------------------------------------------
// 書き込み (docs/crypto-volumes.md「書き込み」): DecryptingDevice の共通部分 (RMW・キャッシュ整合・失敗で読み取り専用へ・flush の素通し・禁止範囲)。
// 暗号方式そのものは cryptwrite_tool と tools/run_crypt_write_tests.sh (cryptsetup / Python cryptography / libbde との照合) が受け持つ。
// ここでは 1 バイトごとに位置依存の鍵 (ks) で XOR する疑似暗号を使い、暗号文は下位 (MemoryDevice) に実際に書かれる。
// ---------------------------------------------------------------------------------------------------------------------------------------------------
namespace {

uint8_t ks(uint64_t pos) { return uint8_t((pos * 197 + (pos >> 7) * 13 + 91) & 0xff); }

// 下位の包み: 書き込み / flush の回数と失敗の注入、書き込みの粒度・順序の記録
class FaultBase final : public BlockDevice {
 public:
  explicit FaultBase(uint64_t size) : mem_(std::make_shared<MemoryDevice>(std::vector<uint8_t>(size, 0))) {}
  Status read_at(uint64_t o, void* b, size_t l, const IoOptions& io = {}) override { return mem_->read_at(o, b, l, io); }
  Status write_at(uint64_t o, const void* b, size_t l, const IoOptions& io = {}) override {
    std::lock_guard<std::mutex> g(mu);
    writes++;
    log.push_back({o, l});
    if (fail_after >= 0 && writes > fail_after) return Status::Make(Errc::Io, 5);
    return mem_->write_at(o, b, l, io);
  }
  Status flush() override { flushes++; return fail_flush ? Status::Make(Errc::Io, 5) : Status::Ok(); }
  Status close() override { return Status::Ok(); }
  uint64_t size() const override { return mem_->size(); }
  uint32_t logical_block_size() const override { return 512; }
  bool is_read_only() const override { return false; }
  std::string describe() const override { return "faultbase"; }
  std::vector<uint8_t>& raw() { return mem_->bytes(); }
  std::mutex mu;
  long writes = 0, flushes = 0, fail_after = -1;
  bool fail_flush = false;
  std::vector<std::pair<uint64_t, size_t>> log;

 private:
  std::shared_ptr<MemoryDevice> mem_;
};

// 疑似暗号デバイス。[0, forbid_len) の論理範囲は書き込み禁止 (BitLocker のメタデータ領域の模擬)。
class XorDevice final : public DecryptingDevice {
 public:
  XorDevice(BlockDevicePtr base, uint64_t size, uint32_t sector, uint32_t cache, bool writable, uint64_t forbid_off = 0, uint64_t forbid_len = 0)
      : DecryptingDevice(base, size, sector, cache, "xor"), forbid_off_(forbid_off), forbid_len_(forbid_len) {
    set_writable(writable);
  }
  bool bad_align = false;
  std::atomic<int> wiped{0};

 protected:
  Status read_plain(uint64_t off, uint8_t* buf, size_t len, const IoOptions& io) override {
    if (off % sector_size() || len % sector_size() || !len) { bad_align = true; return Status::Make(Errc::InvalidArgument); }
    Status st = base_->read_at(off, buf, len, io);
    if (!st) return st;
    for (size_t i = 0; i < len; i++) buf[i] ^= ks(off + i);
    return Status::Ok();
  }
  Status write_plain(uint64_t off, const uint8_t* buf, size_t len, const IoOptions& io) override {
    if (off % sector_size() || len % sector_size() || !len) { bad_align = true; return Status::Make(Errc::InvalidArgument); }
    std::vector<uint8_t> e(buf, buf + len);
    for (size_t i = 0; i < len; i++) e[i] ^= ks(off + i);
    return base_->write_at(off, e.data(), len, io);
  }
  Status check_write_range(uint64_t off, size_t len) const override {
    if (forbid_len_ && off < forbid_off_ + forbid_len_ && forbid_off_ < off + len) return Status::Make(Errc::ReadOnly);
    return Status::Ok();
  }
  void wipe_keys() override { wiped++; }

 private:
  uint32_t sector_size() const { return logical_block_size(); }
  uint64_t forbid_off_, forbid_len_;
};

struct WFix {
  std::shared_ptr<FaultBase> base;
  std::shared_ptr<XorDevice> dev;
  std::vector<uint8_t> model;
  WFix(uint64_t size, uint32_t sector, uint32_t cache = 0, bool writable = true, uint64_t fo = 0, uint64_t fl = 0) {
    base = std::make_shared<FaultBase>(size);
    std::mt19937_64 rng(size + sector);
    model.resize(size - size % sector);
    for (auto& b : model) b = uint8_t(rng());
    for (size_t i = 0; i < model.size(); i++) base->raw()[i] = model[i] ^ ks(i);  // 暗号文を下位に置く
    dev = std::make_shared<XorDevice>(base, size, sector, cache, writable, fo, fl);
  }
  bool read_matches(uint64_t off, size_t len) {
    std::vector<uint8_t> b(len);
    if (!dev->read_at(off, b.data(), len).ok()) return false;
    return std::memcmp(b.data(), model.data() + off, len) == 0;
  }
  // 下位の暗号文を直接復号して、モデルと全体が一致するか (キャッシュを介さない照合)
  bool raw_matches() {
    for (size_t i = 0; i < model.size(); i++)
      if (uint8_t(base->raw()[i] ^ ks(i)) != model[i]) return false;
    return true;
  }
};

}  // namespace

#ifdef USBMEM_CRYPT_WRITE
TEST(transform_write_random_vs_model) {
  for (uint32_t sector : {512u, 4096u}) {
    WFix f(3 * 1024 * 1024 + sector * 5, sector);
    CHECK(!f.dev->is_read_only());
    std::mt19937_64 rng(sector * 3 + 1);
    const uint64_t S = f.dev->size();
    for (int it = 0; it < 1500; it++) {
      // 読み取りでキャッシュを温めてから書く (更新 / 無効化の確認)
      if (rng() % 3 == 0) { uint64_t o = (rng() % (S / 4096)) * 4096; for (int k = 0; k < 8 && o + 4096 <= S; k++, o += 4096) CHECK(f.read_matches(o, 4096)); }
      uint64_t len, off;
      switch (rng() % 6) {
        case 0: len = 1 + rng() % 7; break;
        case 1: len = 1 + rng() % sector; break;
        case 2: len = sector; break;
        case 3: len = 4096 + rng() % 3 - 1; break;
        case 4: len = 1 + rng() % 70000; break;
        default: len = 1 + rng() % (2u << 20); break;  // 1 MiB の塊をまたぐ
      }
      len = std::min<uint64_t>(len, S);
      off = rng() % (S - len + 1);
      if (rng() % 5 == 0) off = S - len;  // 末尾
      std::vector<uint8_t> d(len);
      for (auto& x : d) x = uint8_t(rng());
      Status st = f.dev->write_at(off, d.data(), len);
      CHECK(st.ok());
      std::memcpy(f.model.data() + off, d.data(), len);
      CHECK(f.read_matches(off > 700 ? off - 700 : 0, size_t(std::min<uint64_t>(S - (off > 700 ? off - 700 : 0), len + 1400))));
      if (rng() % 10 == 0) CHECK(f.read_matches((rng() % (S - 100000)), 100000));
    }
    CHECK(f.read_matches(0, size_t(S)));
    CHECK(f.raw_matches());            // 下位には本当に暗号文が書かれている
    CHECK(!f.dev->bad_align);          // 暗号化層へは常にセクタ境界・セクタ倍数で渡る (部分セクタは RMW)
    for (auto& w : f.base->log) CHECK(w.first % sector == 0 && w.second % sector == 0 && w.second <= (1u << 20) + sector);
    CHECK(f.dev->close().ok());
    CHECK(f.base->flushes >= 1);       // close が最後の flush を下位へ渡す
  }
}

// 一部のセクタだけを書いたとき、下位に書かれるのはそのセクタだけ (隣のセクタを書き換えない・書く前に読む)
TEST(transform_write_partial_sector_rmw) {
  WFix f(1 << 20, 4096);
  uint8_t b[10];
  std::memset(b, 0x5a, sizeof b);
  CHECK(f.dev->write_at(4096 * 3 + 100, b, sizeof b).ok());
  CHECK_EQ(f.base->log.size(), 1u);
  CHECK_EQ(f.base->log[0].first, 4096u * 3);
  CHECK_EQ(f.base->log[0].second, 4096u);
  std::memcpy(f.model.data() + 4096 * 3 + 100, b, sizeof b);
  CHECK(f.raw_matches());
  // 2 つのセクタにまたがる小さい書き込み: 2 セクタを 1 回で
  f.base->log.clear();
  CHECK(f.dev->write_at(4096 * 5 - 3, b, sizeof b).ok());
  CHECK_EQ(f.base->log.size(), 1u);
  CHECK_EQ(f.base->log[0].first, 4096u * 4);
  CHECK_EQ(f.base->log[0].second, 8192u);
  std::memcpy(f.model.data() + 4096 * 5 - 3, b, sizeof b);
  CHECK(f.raw_matches());
  CHECK(f.read_matches(4096 * 4, 8192));
  // 0 バイトは何もしない
  const long w = f.base->writes;
  CHECK(f.dev->write_at(77, b, 0).ok());
  CHECK_EQ(f.base->writes, w);
}

// 読み取りで温めたキャッシュが、書き込み後に古い内容を返さない (突然変異: 書き込み後のキャッシュ更新を外すと、この試験が落ちる)
TEST(transform_write_cache_coherence) {
  WFix f(2 << 20, 512);
  CHECK(f.read_matches(0, 1 << 20));  // 先頭 1 MiB をキャッシュ (先読み含む) に載せる…
  for (uint64_t o = 0; o < (512u << 10); o += 4096) CHECK(f.read_matches(o, 4096));
  std::vector<uint8_t> d(10000, 0xc3);
  CHECK(f.dev->write_at(5000, d.data(), d.size()).ok());
  std::memcpy(f.model.data() + 5000, d.data(), d.size());
  CHECK(f.read_matches(4096, 4096));
  CHECK(f.read_matches(8192, 4096));
  CHECK(f.read_matches(0, 4096));
  CHECK(f.read_matches(5000, 10000));
  // 下位が失敗したとき、キャッシュの内容を信用しない (無効化): 失敗した書き込みの範囲は下位から読み直される
  f.base->fail_after = f.base->writes;  // 以後の書き込みは失敗
  std::vector<uint8_t> d2(4096, 0x11);
  CHECK(!f.dev->write_at(8192, d2.data(), d2.size()).ok());
  CHECK(f.dev->is_read_only());
  CHECK(f.read_matches(8192, 4096));  // 下位の (書かれなかった) 内容 = モデルのまま
}

TEST(transform_write_failure_degrades_to_read_only) {
  for (int mode = 0; mode < 3; mode++) {
    WFix f(4 << 20, 512);
    std::vector<uint8_t> d(4096, 0x77);
    CHECK(f.dev->write_at(0, d.data(), d.size()).ok());
    std::memcpy(f.model.data(), d.data(), d.size());
    CHECK(!f.dev->is_read_only());
    Status st;
    if (mode == 0) { f.base->fail_after = f.base->writes; st = f.dev->write_at(8192, d.data(), d.size()); }
    else if (mode == 1) { f.base->fail_flush = true; st = f.dev->flush(); }
    else {  // 3 MiB の書き込みの 2 塊目で失敗: 先頭の塊だけが反映される (エラーが返る・以後は書けない)
      f.base->fail_after = f.base->writes + 1;
      std::vector<uint8_t> big(3 << 20, 0x42);
      st = f.dev->write_at(0, big.data(), big.size());
    }
    CHECK(st.code == Errc::Io);
    CHECK(f.dev->is_read_only());
    const long w = f.base->writes;
    CHECK(f.dev->write_at(0, d.data(), 512).code == Errc::ReadOnly);
    CHECK_EQ(f.base->writes, w);        // 失敗後は下位に触れない
    CHECK(!f.dev->flush().ok());        // flush も「書けた」とは言わない
    std::vector<uint8_t> r(512);
    CHECK(f.dev->read_at(0, r.data(), 512).ok());  // 読み取りは続けられる
    CHECK(f.dev->close().ok());
  }
}

TEST(transform_write_flush_reaches_base) {
  WFix f(1 << 20, 512);
  std::vector<uint8_t> d(100, 1);
  CHECK(f.dev->write_at(1, d.data(), d.size()).ok());
  CHECK_EQ(f.base->flushes, 0);       // 書き込みだけでは flush しない (FS が必要なときだけ flush する)
  CHECK(f.dev->flush().ok());
  CHECK_EQ(f.base->flushes, 1);       // flush は必ず下位へ
  CHECK(f.dev->flush().ok());
  CHECK_EQ(f.base->flushes, 2);
  CHECK(f.dev->close().ok());
  CHECK_EQ(f.base->flushes, 3);       // close は最後にもう一度
  CHECK(f.dev->flush().code == Errc::Closed);
  CHECK(f.dev->write_at(0, d.data(), 1).code == Errc::Closed);
  CHECK_EQ(f.dev->wiped.load(), 1);
}

TEST(transform_write_range_and_forbidden) {
  const uint64_t S = 1 << 20;
  WFix f(S, 512, 0, true, 100000, 5000);  // 論理 [100000, 105000) は書き込み禁止
  std::vector<uint8_t> d(2000, 9);
  CHECK(f.dev->write_at(S - 1000, d.data(), 2000).code == Errc::OutOfRange);
  CHECK(f.dev->write_at(S + 1, d.data(), 1).code == Errc::OutOfRange);
  CHECK(f.dev->write_at(~uint64_t(0) - 3, d.data(), 100).code == Errc::OutOfRange);
  CHECK(f.dev->write_at(S - 2000, d.data(), 2000).ok());
  CHECK(f.dev->write_at(S, d.data(), 0).ok());
  const long w = f.base->writes;
  for (uint64_t off : {uint64_t(99000), uint64_t(100000), uint64_t(104999), uint64_t(100500)}) {  // 掛かるものは全部拒否 (要求の一部だけ書かない)
    CHECK(f.dev->write_at(off, d.data(), 2000).code == Errc::ReadOnly);
    CHECK_EQ(f.base->writes, w);
    CHECK(!f.dev->is_read_only());  // 範囲の拒否で書き込み不可に落ちない
  }
  CHECK(f.dev->write_at(98000, d.data(), 2000).ok());   // 手前で終わる
  CHECK(f.dev->write_at(105000, d.data(), 2000).ok());  // 後ろから始まる
  std::memcpy(f.model.data() + S - 2000, d.data(), 2000);
  std::memcpy(f.model.data() + 98000, d.data(), 2000);
  std::memcpy(f.model.data() + 105000, d.data(), 2000);
  CHECK(f.raw_matches());
}

// 書き込み可を指定しなければ (既定)、書き込みは ReadOnly で下位に何も書かない。is_read_only() も true
TEST(transform_write_default_read_only) {
  WFix f(1 << 20, 512, 0, false);
  CHECK(f.dev->is_read_only());
  std::vector<uint8_t> d(512, 3);
  CHECK(f.dev->write_at(0, d.data(), 512).code == Errc::ReadOnly);
  CHECK(f.dev->write_at(0, d.data(), 0).code == Errc::ReadOnly);
  CHECK(f.dev->flush().ok());
  CHECK_EQ(f.base->writes, 0);
  CHECK_EQ(f.base->flushes, 0);  // 読み取り専用の flush は下位に触れない (従来どおり)
  CHECK(f.raw_matches());
}

TEST(transform_write_threads) {
  const uint64_t S = 4 << 20;
  WFix f(S, 512);
  const int T = 4;
  const uint64_t part = S / T;
  std::atomic<int> bad{0};
  std::vector<std::thread> th;
  std::vector<std::vector<uint8_t>> models(T);
  for (int t = 0; t < T; t++) {
    models[t].assign(f.model.begin() + long(t * part), f.model.begin() + long((t + 1) * part));
    th.emplace_back([&, t] {
      std::mt19937_64 rng(1000 + t);
      std::vector<uint8_t> b;
      for (int it = 0; it < 300; it++) {
        // 自分の担当領域 [t*part, (t+1)*part) だけに書く。ほかのスレッドは同じキャッシュ・同じセクタ境界の近くを読み書きしている。
        // (担当の境目の部分セクタ: 512 の倍数なので共有しない)
        uint64_t len = 1 + rng() % 40000, off = rng() % (part - len + 1);
        b.resize(len);
        for (auto& x : b) x = uint8_t(rng());
        if (!f.dev->write_at(t * part + off, b.data(), len).ok()) { bad++; continue; }
        std::memcpy(models[t].data() + off, b.data(), len);
        std::vector<uint8_t> r(part);
        if (it % 5 == 0) {
          if (!f.dev->read_at(t * part, r.data(), part).ok() || r != models[t]) bad++;
        } else {
          uint64_t l2 = 1 + rng() % 30000, o2 = rng() % (part - l2 + 1);
          r.resize(l2);
          if (!f.dev->read_at(t * part + o2, r.data(), l2).ok() || std::memcmp(r.data(), models[t].data() + o2, l2) != 0) bad++;
        }
      }
    });
  }
  for (auto& x : th) x.join();
  CHECK_EQ(bad.load(), 0);
  for (int t = 0; t < T; t++) std::memcpy(f.model.data() + t * part, models[t].data(), part);
  CHECK(f.read_matches(0, size_t(S)));
  CHECK(f.raw_matches());
}

// 書き込み中に別スレッドが close しても落ちない
TEST(transform_write_close_race) {
  WFix f(2 << 20, 512);
  std::thread wr([&] {
    std::vector<uint8_t> b(30000, 5);
    for (int i = 0; i < 300; i++) f.dev->write_at(uint64_t(i) * 4000, b.data(), b.size());
  });
  f.dev->close();
  wr.join();
  CHECK_EQ(f.dev->wiped.load(), 1);
}
#endif

// OpenOptions の既定は読み取り専用 (書き込みは open 時に明示したときだけ)。ビルドが USBMEM_CRYPT_WRITE でなければ、明示しても書き込み可にならない
// (実暗号方式での確認は tools/run_crypt_write_tests.sh の norw)。
TEST(transform_write_option_default_read_only) {
  OpenOptions o;
  CHECK(!o.writable);
  VolumeInfo v;
  CHECK(!v.write_supported && !v.writable);
}
