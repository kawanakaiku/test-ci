// SPDX-License-Identifier: Apache-2.0
// 暗号ボリューム (LUKS / BitLocker) の書き込みの検証ツール (docs/crypto-volumes.md「書き込み」)。USBMEM_CRYPT_WRITE=ON のビルドで意味を持つ。
// 独立実装 (cryptsetup / dm-crypt、Python cryptography、libbde) との突き合わせはシェル側 (tools/run_crypt_write_tests.sh, .github/workflows/crypt-write.yml)。
//
// 資格情報 <cred>: pw:<パスワード> | pwfile:<ファイル> | rec:<48桁> | key:<ファイル> | none
// 共通オプション (コマンドの後ろ): --protect OFF:LEN  書き込みの前後で 1 バイトも変わってはいけない下位の範囲 (LUKS ヘッダ / BitLocker のメタデータ・ブート領域)
//                                   --hidden OFF:LEN   復号デバイス上で 0 に見えて、書き込みが ReadOnly で拒否されるべき範囲 (BitLocker のメタデータ領域)
//   rand   <image> <cred> <seed> <nops> <model_out> 乱数の書き込み (小・大・セクタ/ページ/チャンクをまたぐ・末尾) を平文のモデルと突き合わせる。キャッシュを温めてから書く
//                                                   / 途中の flush が下位へ届く / 閉じて開き直した後も全体一致 / 保護範囲不変。最終的な平文を model_out に書く
//   verify <image> <cred> <model>                  復号デバイス全体がモデル (ファイル) と一致するか (読み取り専用で開く)
//   ro     <image> <cred>                          既定 (writable 指定なし) では is_read_only()、書き込みは ReadOnly、イメージ不変
//   norw   <image> <cred>                          writable を要求しても書き込み非対応の構成なら、読み取り専用で開き write_note がある・書き込みは ReadOnly・イメージ不変
//   info   <image> <cred> [--writable]             probe / open の結果 (write_supported / writable / write_note) を表示
//   failinj <image> <cred> <seed>                  下位の書き込み / flush を途中で失敗させる: エラーを返し、以後の書き込みは ReadOnly (読み取りは続く)
//   fuzzw  <image> <cred> <iters> <seed>           ヘッダを壊して書き込み可で開く。クラッシュ・ハングしない。開けたら書き込んでも保護範囲は不変
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "usbmem/devices.h"
#include "usbmem/transform.h"

using namespace usbmem;

namespace {

struct Range { uint64_t off, len; };
struct Opts {
  std::vector<Range> protect, hidden;
};
int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)

std::vector<uint8_t> read_file(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool parse_cred(const std::string& spec, Credentials* c) {
  auto set = [](crypto::Secret& s, const std::string& v) { s.assign(reinterpret_cast<const uint8_t*>(v.data()), v.size()); };
  if (spec == "none") return true;
  size_t colon = spec.find(':');
  if (colon == std::string::npos) return false;
  std::string k = spec.substr(0, colon), v = spec.substr(colon + 1);
  if (k == "pw") set(c->password, v);
  else if (k == "pwfile") { auto b = read_file(v); while (!b.empty() && (b.back() == '\n' || b.back() == '\r')) b.pop_back(); c->password.assign(b.data(), b.size()); }
  else if (k == "rec") set(c->recovery_key, v);
  else if (k == "key") { auto b = read_file(v); if (b.empty()) return false; c->key_file.assign(b.data(), b.size()); }
  else return false;
  return true;
}

bool parse_range(const char* s, Range* r) {
  char* e = nullptr;
  r->off = std::strtoull(s, &e, 0);
  if (!e || *e != ':') return false;
  r->len = std::strtoull(e + 1, nullptr, 0);
  return true;
}

// 下位デバイスの包み: 書き込み / flush の回数を数え、N 回目で失敗させる。
class Probe final : public BlockDevice {
 public:
  explicit Probe(BlockDevicePtr p) : p_(std::move(p)) {}
  Status read_at(uint64_t o, void* b, size_t l, const IoOptions& io = {}) override { reads++; return p_->read_at(o, b, l, io); }
  Status write_at(uint64_t o, const void* b, size_t l, const IoOptions& io = {}) override {
    writes++;
    if (fail_write_at >= 0 && writes >= fail_write_at) return Status::Make(Errc::Io, 5);
    return p_->write_at(o, b, l, io);
  }
  Status flush() override {
    flushes++;
    if (fail_flush) return Status::Make(Errc::Io, 5);
    return p_->flush();
  }
  Status close() override { return p_->close(); }
  uint64_t size() const override { return p_->size(); }
  uint32_t logical_block_size() const override { return p_->logical_block_size(); }
  bool is_read_only() const override { return p_->is_read_only(); }
  std::string describe() const override { return "probe(" + p_->describe() + ")"; }
  long writes = 0, flushes = 0, reads = 0, fail_write_at = -1;
  bool fail_flush = false;

 private:
  BlockDevicePtr p_;
};

std::shared_ptr<Probe> open_raw(const std::string& img, bool ro) {
  BlockDevicePtr raw;
  if (!RawImageDevice::open_path(img, ro, 512, &raw).ok()) return nullptr;
  return std::make_shared<Probe>(raw);
}

Status open_vol(const std::shared_ptr<Probe>& base, const std::string& cred, bool writable, BlockDevicePtr* out, VolumeInfo* info) {
  Credentials c;
  if (!parse_cred(cred, &c)) return Status::Make(Errc::InvalidArgument);
  OpenOptions o;
  o.writable = writable;
  return open_crypto_volume(base, CryptoKind::Auto, c, o, out, info);
}

std::vector<uint8_t> snapshot(const std::string& img, const std::vector<Range>& rs) {
  std::vector<uint8_t> all;
  std::ifstream f(img, std::ios::binary);
  for (const Range& r : rs) {
    std::vector<uint8_t> b(r.len, 0);
    f.clear();
    f.seekg(std::streamoff(r.off));
    f.read(reinterpret_cast<char*>(b.data()), std::streamsize(r.len));
    all.insert(all.end(), b.begin(), b.end());
  }
  return all;
}

bool read_all(BlockDevice& d, std::vector<uint8_t>* out) {
  out->assign(size_t(d.size()), 0);
  const size_t step = 1u << 20;
  for (uint64_t o = 0; o < d.size(); o += step) {
    size_t n = size_t(std::min<uint64_t>(step, d.size() - o));
    if (!d.read_at(o, out->data() + o, n).ok()) return false;
  }
  return true;
}

bool hits(const std::vector<Range>& rs, uint64_t off, uint64_t len) {
  for (const Range& r : rs) if (off < r.off + r.len && r.off < off + len) return true;
  return false;
}

// ---------------------------------------------------------------- rand
int cmd_rand(const std::string& img, const std::string& cred, uint64_t seed, int nops, const std::string& model_out, const Opts& op) {
  auto base = open_raw(img, false);
  if (!base) { std::fprintf(stderr, "cannot open %s\n", img.c_str()); return 2; }
  const auto prot_before = snapshot(img, op.protect);
  BlockDevicePtr dev;
  VolumeInfo info;
  Status st = open_vol(base, cred, true, &dev, &info);
  if (!st.ok()) { std::fprintf(stderr, "open failed: %s (%s)\n", errc_name(st.code), info.note.c_str()); return 2; }
  if (dev->is_read_only() || !info.writable) { std::fprintf(stderr, "not writable: %s\n", info.write_note.c_str()); return 3; }
  const uint64_t S = dev->size();
  const uint32_t sec = dev->logical_block_size();
  std::vector<uint8_t> model;
  if (!read_all(*dev, &model)) { std::fprintf(stderr, "initial read failed\n"); return 2; }
  std::mt19937_64 rng(seed);
  auto rnd = [&](uint64_t n) { return n ? rng() % n : 0; };
  long ok_writes = 0, rejected = 0, reads = 0, flushes = 0;
  std::vector<uint8_t> buf, got;
  for (int i = 0; i < nops && g_fail == 0; i++) {
    uint64_t off, len;
    switch (rnd(10)) {
      case 0: len = 1 + rnd(8); off = rnd(S - len + 1); break;
      case 1: len = 1 + rnd(sec - 1); off = rnd(S - len + 1); break;
      case 2: len = sec; off = rnd(S / sec) * sec; break;
      case 3: len = sec - 1 + rnd(3); off = rnd(S - len + 1); break;
      case 4: len = 4096; off = rnd(S / 4096) * 4096; break;
      case 5: { len = 1 + rnd(9000); uint64_t k = 1 + rnd(S / 4096 - 1); off = k * 4096 - std::min<uint64_t>(len, 1 + rnd(len)); break; }
      case 6: len = 1 + rnd(65536); off = rnd(S - len + 1); break;
      case 7: len = (1 + rnd(300 * 1024 / sec)) * sec; off = rnd((S - len) / sec + 1) * sec; break;
      case 8: len = 1 + rnd(3u << 20); off = rnd(S - len + 1); break;  // 1 MiB の塊を何度もまたぐ
      default: len = 1 + rnd(2 * sec); off = S - len; break;          // 末尾
    }
    if (len > S) len = S;
    if (off + len > S) off = S - len;
    // キャッシュを温めてから書く (書き込みがキャッシュを正しく更新 / 無効化するか)
    if (rnd(3) == 0) {
      uint64_t w = rnd(S / 4096) * 4096;
      for (int k = 0; k < 24 && w + 4096 <= S; k++, w += 4096) {
        got.assign(4096, 0);
        CHECK(dev->read_at(w, got.data(), 4096).ok());
        CHECK(std::memcmp(got.data(), model.data() + w, 4096) == 0);
      }
      off = std::min<uint64_t>(off, S - len);
      if (rnd(2)) { off = std::min<uint64_t>(w > 40000 ? w - 40000 + rnd(30000) : 0, S - len); }
    }
    buf.resize(size_t(len));
    switch (rnd(8)) {
      case 0: std::fill(buf.begin(), buf.end(), 0); break;
      case 1: std::fill(buf.begin(), buf.end(), 0xff); break;
      default: for (auto& x : buf) x = uint8_t(rng()); break;
    }
    const bool in_hidden = hits(op.hidden, off, len);
    const auto base_writes = base->writes;
    st = dev->write_at(off, buf.data(), size_t(len));
    if (in_hidden) {
      CHECK(st.code == Errc::ReadOnly);
      CHECK(base->writes == base_writes);   // 下位へは何も書かない
      CHECK(!dev->is_read_only());          // 範囲の拒否で書き込み不可に落ちない
      rejected++;
    } else {
      if (!st.ok()) { std::fprintf(stderr, "op %d write(off=%llu len=%llu) failed: %s\n", i, (unsigned long long)off, (unsigned long long)len, errc_name(st.code)); g_fail++; break; }
      std::memcpy(model.data() + off, buf.data(), size_t(len));
      ok_writes++;
    }
    // 直後の読み戻し (書いた範囲とその周り)
    {
      uint64_t a = off > 600 ? off - 600 : 0, b = std::min<uint64_t>(S, off + len + 600);
      got.assign(size_t(b - a), 0);
      CHECK(dev->read_at(a, got.data(), got.size()).ok());
      CHECK(std::memcmp(got.data(), model.data() + a, got.size()) == 0);
      reads++;
    }
    if (rnd(4) == 0) {
      uint64_t l2 = 1 + rnd(200000), o2 = rnd(S - std::min(l2, S) + 1);
      l2 = std::min(l2, S - o2);
      got.assign(size_t(l2), 0);
      CHECK(dev->read_at(o2, got.data(), size_t(l2)).ok());
      CHECK(std::memcmp(got.data(), model.data() + o2, size_t(l2)) == 0);
      reads++;
    }
    if (rnd(8) == 0) {
      const long f0 = base->flushes;
      CHECK(dev->flush().ok());
      CHECK(base->flushes == f0 + 1);  // flush は必ず下位へ届く
      flushes++;
    }
  }
  if (g_fail) { std::fprintf(stderr, "stopped at failure\n"); return 1; }
  CHECK(dev->flush().ok());
  // 全体を読み直して一致 (キャッシュ経由でない直接経路・キャッシュ経由の両方)
  {
    std::vector<uint8_t> all;
    CHECK(read_all(*dev, &all));
    CHECK(all == model);
    for (uint64_t o = 0; o < S && g_fail == 0; o += 4096) {  // 4 KiB ずつ (キャッシュ・先読み経路)
      size_t n = size_t(std::min<uint64_t>(4096, S - o));
      got.assign(n, 0);
      CHECK(dev->read_at(o, got.data(), n).ok());
      if (std::memcmp(got.data(), model.data() + o, n) != 0) { std::fprintf(stderr, "cached read mismatch at %llu\n", (unsigned long long)o); g_fail++; break; }
    }
  }
  const long bw = base->writes;
  CHECK(dev->close().ok());
  CHECK(dev->write_at(0, model.data(), 1).code == Errc::Closed);
  dev.reset();
  base.reset();
  CHECK(snapshot(img, op.protect) == prot_before);  // ヘッダ / メタデータは 1 バイトも変わらない
  // 開き直す (読み取り専用で): 下位に実際に暗号文が書かれている
  {
    auto b2 = open_raw(img, true);
    BlockDevicePtr d2;
    VolumeInfo i2;
    CHECK(b2 && open_vol(b2, cred, false, &d2, &i2).ok());
    if (d2) {
      CHECK(d2->is_read_only());
      std::vector<uint8_t> all;
      CHECK(read_all(*d2, &all));
      CHECK(all == model);
    }
  }
  if (!model_out.empty()) {
    std::ofstream f(model_out, std::ios::binary | std::ios::trunc);
    f.write(reinterpret_cast<const char*>(model.data()), std::streamsize(model.size()));
    CHECK(bool(f));
  }
  std::printf("rand: ok (%s, %s) size=%llu sector=%u ops=%d writes=%ld rejected=%ld reads=%ld flushes=%ld base_writes=%ld\n", info.format.c_str(), info.cipher.c_str(),
              (unsigned long long)S, sec, nops, ok_writes, rejected, reads, flushes, bw);
  return g_fail ? 1 : 0;
}

// ---------------------------------------------------------------- verify
int cmd_verify(const std::string& img, const std::string& cred, const std::string& model_path) {
  auto base = open_raw(img, true);
  if (!base) return 2;
  BlockDevicePtr dev;
  VolumeInfo info;
  Status st = open_vol(base, cred, false, &dev, &info);
  if (!st.ok()) { std::fprintf(stderr, "open failed: %s\n", errc_name(st.code)); return 2; }
  auto model = read_file(model_path);
  std::vector<uint8_t> all;
  if (!read_all(*dev, &all)) return 2;
  if (all.size() != model.size()) { std::fprintf(stderr, "size mismatch: %zu vs %zu\n", all.size(), model.size()); return 1; }
  for (size_t i = 0; i < all.size(); i++)
    if (all[i] != model[i]) { std::fprintf(stderr, "first difference at %zu\n", i); return 1; }
  std::printf("verify: ok (%zu bytes)\n", all.size());
  return 0;
}

// ---------------------------------------------------------------- ro / norw
int cmd_ro(const std::string& img, const std::string& cred, bool request_writable) {
  const auto before = read_file(img);
  {
    auto base = open_raw(img, false);  // 下位は書き込み可で開く: 拒否は暗号層の側の判断であることを見る
    if (!base) return 2;
    BlockDevicePtr dev;
    VolumeInfo info;
    Status st = open_vol(base, cred, request_writable, &dev, &info);
    if (!st.ok()) { std::fprintf(stderr, "open failed: %s\n", errc_name(st.code)); return 2; }
    CHECK(dev->is_read_only());
    CHECK(!info.writable);
    if (request_writable) CHECK(!info.write_note.empty());
    std::vector<uint8_t> b(4096, 0xA5);
    CHECK(dev->write_at(0, b.data(), 512).code == Errc::ReadOnly);
    CHECK(dev->write_at(dev->size() - 512, b.data(), 512).code == Errc::ReadOnly);
    CHECK(dev->write_at(100, b.data(), 7).code == Errc::ReadOnly);
    CHECK(dev->flush().ok());
    CHECK(base->writes == 0);
    CHECK(dev->read_at(0, b.data(), 512).ok());
    std::printf("%s: is_read_only=%d writable=%d write_supported=%d write_note=%s\n", request_writable ? "norw" : "ro", dev->is_read_only(), info.writable, info.write_supported,
                info.write_note.c_str());
  }
  CHECK(read_file(img) == before);
  return g_fail ? 1 : 0;
}

int cmd_info(const std::string& img, const std::string& cred, bool writable) {
  auto base = open_raw(img, !writable);
  if (!base) return 2;
  VolumeInfo p;
  Status ps = probe_crypto_volume(*base, &p);
  std::printf("probe: %s kind=%d format=%s cipher=%s supported=%d write_supported=%d writable=%d write_note=%s\n", errc_name(ps.code), int(p.kind), p.format.c_str(), p.cipher.c_str(),
              p.supported, p.write_supported, p.writable, p.write_note.c_str());
  BlockDevicePtr dev;
  VolumeInfo info;
  Status st = open_vol(base, cred, writable, &dev, &info);
  std::printf("open: %s writable=%d is_read_only=%d write_note=%s\n", errc_name(st.code), info.writable, dev ? int(dev->is_read_only()) : -1, info.write_note.c_str());
  return st.ok() ? 0 : 1;
}

// ---------------------------------------------------------------- failinj
int cmd_failinj(const std::string& img, const std::string& cred, uint64_t seed, const Opts& op) {
  for (int mode = 0; mode < 3; mode++) {  // 0: N 回目の書き込みが失敗、1: flush が失敗、2: 複数チャンクの途中で失敗
    auto base = open_raw(img, false);
    if (!base) return 2;
    const auto prot_before = snapshot(img, op.protect);
    BlockDevicePtr dev;
    VolumeInfo info;
    Status st = open_vol(base, cred, true, &dev, &info);
    if (!st.ok() || dev->is_read_only()) { std::fprintf(stderr, "not writable\n"); return 3; }
    std::vector<uint8_t> before;
    CHECK(read_all(*dev, &before));
    std::mt19937_64 rng(seed + uint64_t(mode));
    std::vector<uint8_t> b(3u << 20);
    for (auto& x : b) x = uint8_t(rng());
    // まず成功する書き込み
    CHECK(dev->write_at(8192 + 3, b.data(), 5000).ok());
    CHECK(!dev->is_read_only());
    if (mode == 0) {
      base->fail_write_at = base->writes + 1;
      st = dev->write_at(40000, b.data(), 1000);
      CHECK(!st.ok() && st.code == Errc::Io);
    } else if (mode == 1) {
      base->fail_flush = true;
      st = dev->flush();
      CHECK(!st.ok() && st.code == Errc::Io);
    } else {
      base->fail_write_at = base->writes + 2;  // 3 MiB = 3 塊 (以上): 2 塊目で失敗
      st = dev->write_at(70000, b.data(), std::min<size_t>(b.size(), size_t(dev->size() - 70000)));  // 3 MiB 弱 (ボリュームは 4 MiB 以上であること)
      CHECK(!st.ok() && st.code == Errc::Io);
    }
    CHECK(dev->is_read_only());                                   // 読み取り専用に落ちた
    const long w = base->writes;
    CHECK(dev->write_at(0, b.data(), 512).code == Errc::ReadOnly);  // 以後の書き込みは拒否
    CHECK(base->writes == w);                                       // 下位へは書かない
    CHECK(!dev->flush().ok());                                      // 「確実に書けた」とは言わない
    std::vector<uint8_t> a(4096);
    CHECK(dev->read_at(0, a.data(), a.size()).ok());               // 読み取りは続けられる
    CHECK(dev->close().ok());
    dev.reset();
    base.reset();
    CHECK(snapshot(img, op.protect) == prot_before);
    std::printf("failinj mode %d: ok\n", mode);
  }
  return g_fail ? 1 : 0;
}

// ---------------------------------------------------------------- fuzzw
int cmd_fuzzw(const std::string& img, const std::string& cred, int iters, uint64_t seed, const Opts& op) {
  std::vector<uint8_t> orig = read_file(img);
  if (orig.empty()) return 2;
  std::mt19937_64 rng(seed);
  const std::string work = img + ".fuzzw";
  int opened = 0, writable_n = 0;
  for (int it = 0; it < iters && g_fail == 0; it++) {
    std::vector<uint8_t> m = orig;
    int nmut = 1 + int(rng() % 6);
    const size_t hot = std::min<size_t>(m.size(), size_t(op.protect.empty() ? 65536 : op.protect[0].len + 4096));
    for (int k = 0; k < nmut; k++) {
      size_t pos = (rng() % 3 == 0) ? size_t(rng() % m.size()) : size_t(rng() % hot);
      switch (rng() % 3) {
        case 0: m[pos] ^= uint8_t(1u << (rng() % 8)); break;
        case 1: m[pos] = uint8_t(rng()); break;
        default: for (size_t j = 0; j < 16 && pos + j < m.size(); j++) m[pos + j] = uint8_t(rng()); break;
      }
    }
    if (rng() % 7 == 0) m.resize(size_t(rng() % m.size()));
    { std::ofstream f(work, std::ios::binary | std::ios::trunc); f.write(reinterpret_cast<const char*>(m.data()), std::streamsize(m.size())); }
    auto base = open_raw(work, false);
    if (!base) continue;
    const auto prot_before = snapshot(work, op.protect);
    BlockDevicePtr dev;
    VolumeInfo info;
    Status st = open_vol(base, cred, true, &dev, &info);
    if (!st.ok() || !dev) continue;
    opened++;
    if (dev->is_read_only()) continue;
    writable_n++;
    std::vector<uint8_t> b(70000);
    for (auto& x : b) x = uint8_t(rng());
    for (int k = 0; k < 6; k++) {
      uint64_t len = 1 + rng() % b.size();
      if (len > dev->size()) len = dev->size();
      uint64_t off = rng() % (dev->size() - len + 1);
      dev->write_at(off, b.data(), size_t(len));  // 結果は問わない (クラッシュ・ハングしないこと)
      std::vector<uint8_t> r(len);
      dev->read_at(off, r.data(), size_t(len));
    }
    dev->flush();
    dev->close();
    dev.reset();
    base.reset();
    if (snapshot(work, op.protect) != prot_before) { std::fprintf(stderr, "iteration %d: protected range changed\n", it); g_fail++; }
  }
  ::unlink(work.c_str());
  std::printf("fuzzw: %d iterations, opened=%d writable=%d, no crash\n", iters, opened, writable_n);
  return g_fail ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: see the header comment of cryptwrite_tool.cpp\n"); return 64; }
  std::vector<std::string> pos;
  Opts op;
  bool writable_flag = false;
  for (int i = 2; i < argc; i++) {
    std::string a = argv[i];
    Range r;
    if ((a == "--protect" || a == "--hidden") && i + 1 < argc) {
      if (!parse_range(argv[++i], &r)) { std::fprintf(stderr, "bad range\n"); return 64; }
      (a == "--protect" ? op.protect : op.hidden).push_back(r);
    } else if (a == "--writable") writable_flag = true;
    else pos.push_back(a);
  }
  const std::string cmd = argv[1];
  auto n = [&](size_t k) { return pos.size() >= k; };
  if (cmd == "rand" && n(5)) return cmd_rand(pos[0], pos[1], std::strtoull(pos[2].c_str(), nullptr, 0), std::atoi(pos[3].c_str()), pos[4], op);
  if (cmd == "verify" && n(3)) return cmd_verify(pos[0], pos[1], pos[2]);
  if (cmd == "ro" && n(2)) return cmd_ro(pos[0], pos[1], false);
  if (cmd == "norw" && n(2)) return cmd_ro(pos[0], pos[1], true);
  if (cmd == "info" && n(2)) return cmd_info(pos[0], pos[1], writable_flag);
  if (cmd == "failinj" && n(3)) return cmd_failinj(pos[0], pos[1], std::strtoull(pos[2].c_str(), nullptr, 0), op);
  if (cmd == "fuzzw" && n(4)) return cmd_fuzzw(pos[0], pos[1], std::atoi(pos[2].c_str()), std::strtoull(pos[3].c_str(), nullptr, 0), op);
  std::fprintf(stderr, "bad arguments\n");
  return 64;
}
