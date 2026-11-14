// SPDX-License-Identifier: Apache-2.0
// 暗号ボリューム (BitLocker / LUKS) の読み取り速度のマイクロベンチ (docs/crypto-volumes.md 6.4)。
//
// 実機の USB を模した「模擬デバイス」(1 往復あたり固定時間 + 帯域上限 + 1 要求の最大長) を下位に挟み、復号層に次の負荷をかけて
// 下位への要求回数・バイト数・CPU 時間・模擬 I/O 時間・実効速度を出す。I/O の待ちは仮想時間で数える (実際には眠らない) ので、
// 1 GiB 相当でも数秒で測れる。実効速度 = 読んだバイト / (CPU 時間 + 模擬 I/O 時間) (復号と USB 転送が重ならない最悪側の見積もり)。
//
//   usbmem_transform_bench fs <image> <off> <len> [opts]           (btrfs 対応ビルドのみ) Btrfs を模擬デバイス越しにマウントして全ファイルを読む
//       opts は vol と同じ + --no-ra (アプリが下に挟む先読み ReadAheadDevice を使わない)
//   usbmem_transform_bench prims                                   暗号・ハッシュ・CRC のプリミティブ速度 (HW 命令の有無も表示)
//   usbmem_transform_bench vol <image> <off> <len> <cred> [opts]   復号ボリュームの負荷試験。<cred> は pw:<文字列> | pwfile:<ファイル> | rec:<48桁>
//       --lat-us N      1 往復の固定遅延 (既定 1000 = 1 ms)
//       --bw MBPS       帯域上限 (既定 30)
//       --maxreq BYTES  1 回の USB 転送の最大長 (既定 524288)。これを超える要求は複数の往復になる
//       --total MIB     連続読みの量 (既定 16)
//       --files N       小ファイル負荷の件数 (既定 400)
//       --small-only    連続読みを省く
//       --no-hw         AES / SHA-256 の命令版を使わない (表引き実装で測る)
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <mutex>
#include <random>
#include <string>
#include <vector>

#include "usbmem/crc32.h"
#include "usbmem/devices.h"
#include "usbmem/transform.h"
#ifdef USBMEM_BENCH_BTRFS
#include "usbmem/btrfs_volume.h"
#endif

using namespace usbmem;
using Clock = std::chrono::steady_clock;

namespace {

volatile uint32_t g_sink;
double since(Clock::time_point t) { return std::chrono::duration<double>(Clock::now() - t).count(); }

// 下位デバイスの読み取りだけを数えて仮想時間を進める (書き込みは素通し)
class SimDevice final : public BlockDevice {
 public:
  SimDevice(BlockDevicePtr base, double lat_s, double bw, size_t max_req) : base_(std::move(base)), lat_(lat_s), bw_(bw), max_req_(max_req) {}
  Status read_at(uint64_t off, void* buf, size_t len, const IoOptions& opt = {}) override {
    {
      std::lock_guard<std::mutex> g(mu_);
      size_t left = len;
      while (left) {
        size_t n = left < max_req_ ? left : max_req_;
        sim_ += lat_ + double(n) / bw_;
        trips_++;
        left -= n;
      }
      reads_++;
      bytes_ += len;
    }
    return base_->read_at(off, buf, len, opt);
  }
  Status write_at(uint64_t o, const void* b, size_t l, const IoOptions& opt = {}) override { return base_->write_at(o, b, l, opt); }
  Status flush() override { return base_->flush(); }
  Status close() override { return Status::Ok(); }
  uint64_t size() const override { return base_->size(); }
  uint32_t logical_block_size() const override { return base_->logical_block_size(); }
  bool is_read_only() const override { return true; }
  std::string describe() const override { return "sim"; }
  void reset() { std::lock_guard<std::mutex> g(mu_); sim_ = 0; trips_ = reads_ = bytes_ = 0; }
  double sim() const { return sim_; }
  uint64_t trips() const { return trips_; }
  uint64_t reads() const { return reads_; }
  uint64_t bytes() const { return bytes_; }

 private:
  BlockDevicePtr base_;
  double lat_, bw_;
  size_t max_req_;
  std::mutex mu_;
  double sim_ = 0;
  uint64_t trips_ = 0, reads_ = 0, bytes_ = 0;
};

struct Cfg {
  double lat_s = 0.001, bw = 30e6;
  size_t max_req = 524288;
  uint64_t total = 16u << 20;
  int files = 400;
  bool small_only = false;
};

void report(const char* name, SimDevice& sim, BlockDevice& top, double cpu, uint64_t want_bytes) {
  (void)top;
  const double tot = cpu + sim.sim();
  std::printf("  %-34s 下位 %6llu 回/%7.2f MB 転送 %6llu 往復  CPU %6.3f s + I/O %6.3f s  => %7.2f MB/s (要求 %.2f MB)\n", name,
              (unsigned long long)sim.reads(), double(sim.bytes()) / 1e6, (unsigned long long)sim.trips(), cpu, sim.sim(),
              tot > 0 ? double(want_bytes) / 1e6 / tot : 0.0, double(want_bytes) / 1e6);
}

// 連続読み
void bench_seq(const char* label, BlockDevice& dev, SimDevice& sim, size_t req, uint64_t total) {
  std::vector<uint8_t> buf(req);
  sim.reset();
  auto t = Clock::now();
  uint64_t done = 0;
  uint64_t limit = std::min<uint64_t>(total, dev.size());
  for (uint64_t off = 0; off + req <= limit; off += req) {
    if (!dev.read_at(off, buf.data(), req)) { std::printf("  read error at %llu\n", (unsigned long long)off); return; }
    done += req;
  }
  report(label, sim, dev, since(t), done);
}

// ランダム 4 KiB
void bench_rand(const char* label, BlockDevice& dev, SimDevice& sim, int n) {
  std::mt19937_64 rng(1234);
  std::vector<uint8_t> buf(4096);
  uint64_t blocks = dev.size() / 4096;
  sim.reset();
  auto t = Clock::now();
  for (int i = 0; i < n; i++)
    if (!dev.read_at((rng() % blocks) * 4096, buf.data(), 4096)) { std::printf("  read error\n"); return; }
  report(label, sim, dev, since(t), uint64_t(n) * 4096);
}

// 小ファイル: ファイルごとに「メタデータ 4 KiB (ランダム) → データ 4 KiB × k (連続、別の場所)」。
// FS が小さい読みを順に出す様子の模擬 (NTFS / Btrfs で 4 KiB 単位の読みが続く典型)。
void bench_files(const char* label, BlockDevice& dev, SimDevice& sim, int nfiles, int k) {
  std::mt19937_64 rng(99);
  std::vector<uint8_t> buf(4096);
  uint64_t blocks = dev.size() / 4096;
  sim.reset();
  auto t = Clock::now();
  uint64_t want = 0;
  for (int f = 0; f < nfiles; f++) {
    if (!dev.read_at((rng() % blocks) * 4096, buf.data(), 4096)) { std::printf("  read error\n"); return; }
    want += 4096;
    uint64_t b = rng() % (blocks - uint64_t(k));
    for (int i = 0; i < k; i++) {
      if (!dev.read_at((b + uint64_t(i)) * 4096, buf.data(), 4096)) { std::printf("  read error\n"); return; }
      want += 4096;
    }
  }
  report(label, sim, dev, since(t), want);
}

void suite(const char* title, BlockDevice& dev, SimDevice& sim, const Cfg& c) {
  std::printf("[%s]\n", title);
  if (!c.small_only) {
    bench_seq("連続 1 MiB 要求", dev, sim, 1u << 20, c.total);
    bench_seq("連続 64 KiB 要求", dev, sim, 64u << 10, c.total);
    bench_seq("連続 4 KiB 要求 (最大 4 MiB)", dev, sim, 4096, std::min<uint64_t>(c.total, 4u << 20));
  }
  bench_rand("ランダム 4 KiB × 500", dev, sim, 500);
  bench_files("小ファイル (4K メタ + 4K×2)", dev, sim, c.files, 2);
  bench_files("小ファイル (4K メタ + 4K×8)", dev, sim, c.files, 8);
}

bool parse_cred(const std::string& spec, Credentials* c) {
  auto set = [](crypto::Secret& s, const std::string& v) { s.assign(reinterpret_cast<const uint8_t*>(v.data()), v.size()); };
  size_t colon = spec.find(':');
  if (colon == std::string::npos) return false;
  std::string k = spec.substr(0, colon), v = spec.substr(colon + 1);
  if (k == "pw") set(c->password, v);
  else if (k == "pwfile") {
    std::ifstream f(v, std::ios::binary);
    std::string b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    while (!b.empty() && (b.back() == '\n' || b.back() == '\r')) b.pop_back();
    set(c->password, b);
  } else if (k == "rec") set(c->recovery_key, v);
  else return false;
  return true;
}

int do_vol(int argc, char** argv) {
  if (argc < 6) { std::fprintf(stderr, "usage: vol <image> <off> <len> <cred> [opts]\n"); return 2; }
  Cfg c;
  for (int i = 6; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() { return i + 1 < argc ? argv[++i] : (char*)"0"; };
    if (a == "--lat-us") c.lat_s = atof(next()) / 1e6;
    else if (a == "--bw") c.bw = atof(next()) * 1e6;
    else if (a == "--maxreq") c.max_req = size_t(atoll(next()));
    else if (a == "--total") c.total = uint64_t(atoll(next())) << 20;
    else if (a == "--files") c.files = atoi(next());
    else if (a == "--small-only") c.small_only = true;
#ifndef BENCH_BASE
    else if (a == "--no-hw") { crypto::Aes::set_hw_enabled(false); crypto::sha256_set_hw_enabled(false); }
#endif
  }
  BlockDevicePtr raw;
  if (!RawImageDevice::open_path(argv[2], true, 512, &raw)) { std::fprintf(stderr, "open failed\n"); return 1; }
  uint64_t off = strtoull(argv[3], nullptr, 0), len = strtoull(argv[4], nullptr, 0);
  if (len == 0) len = raw->size() - off;
  BlockDevicePtr sub;
  if (!SubRangeDevice::create(raw, off, len, true, &sub)) { std::fprintf(stderr, "range failed\n"); return 1; }
  Credentials cred;
  if (!parse_cred(argv[5], &cred)) { std::fprintf(stderr, "bad cred\n"); return 2; }
  std::printf("模擬デバイス: 往復 %.0f us / 帯域 %.0f MB/s / 1 転送 最大 %zu B\n", c.lat_s * 1e6, c.bw / 1e6, c.max_req);

  auto simp = std::make_shared<SimDevice>(sub, c.lat_s, c.bw, c.max_req);
  suite("素のデバイス (復号なし。上限の目安)", *simp, *simp, c);

  OpenOptions opt;
  BlockDevicePtr dev;
  VolumeInfo info;
  simp->reset();
  auto t = Clock::now();
  Status s = open_crypto_volume(simp, CryptoKind::Auto, cred, opt, &dev, &info);
  double open_s = since(t);
  if (!s) { std::fprintf(stderr, "open failed: code=%d (%s)\n", int(s.code), info.note.c_str()); return 1; }
  std::printf("解錠 (鍵導出込み): CPU %.3f s + 下位 I/O %.3f s (%llu 回 / %llu 往復)  [%s / %s]\n", open_s, simp->sim(), (unsigned long long)simp->reads(),
              (unsigned long long)simp->trips(), info.format.c_str(), info.cipher.c_str());
  suite("復号ボリューム", *dev, *simp, c);
  dev->close();
  return 0;
}


#ifdef USBMEM_BENCH_BTRFS
// Btrfs を全部読む: ディレクトリを辿り、すべての通常ファイルを 128 KiB ずつ読む (アプリの検証テストと同じ形)。
int do_fs(int argc, char** argv) {
  if (argc < 5) { std::fprintf(stderr, "usage: fs <image> <off> <len> [opts]\n"); return 2; }
  Cfg c;
  bool ra = true;
  for (int i = 5; i < argc; i++) {
    std::string a = argv[i];
    auto next = [&]() { return i + 1 < argc ? argv[++i] : (char*)"0"; };
    if (a == "--lat-us") c.lat_s = atof(next()) / 1e6;
    else if (a == "--bw") c.bw = atof(next()) * 1e6;
    else if (a == "--maxreq") c.max_req = size_t(atoll(next()));
    else if (a == "--no-ra") ra = false;
  }
  BlockDevicePtr raw;
  if (!RawImageDevice::open_path(argv[2], true, 512, &raw)) { std::fprintf(stderr, "open failed\n"); return 1; }
  uint64_t off = strtoull(argv[3], nullptr, 0), len = strtoull(argv[4], nullptr, 0);
  if (len == 0) len = raw->size() - off;
  BlockDevicePtr sub;
  if (!SubRangeDevice::create(raw, off, len, true, &sub)) { std::fprintf(stderr, "range failed\n"); return 1; }
  auto simp = std::make_shared<SimDevice>(sub, c.lat_s, c.bw, c.max_req);
  BlockDevicePtr top = simp;
  if (ra) top = ReadAheadDevice::wrap(simp, 256 * 1024);
  std::printf("模擬デバイス: 往復 %.0f us / 帯域 %.0f MB/s / 1 転送 最大 %zu B / 先読み %s\n", c.lat_s * 1e6, c.bw / 1e6, c.max_req, ra ? "有" : "無");
  int err = 0;
  auto t = Clock::now();
  auto vol = BtrfsVolume::mount(top, &err);
  if (!vol) { std::fprintf(stderr, "mount failed: %d\n", err); return 1; }
  std::printf("マウント: CPU %.3f s + I/O %.3f s (%llu 回)\n", since(t), simp->sim(), (unsigned long long)simp->reads());
  simp->reset();
  t = Clock::now();
  uint64_t files = 0, bytes = 0, small_files = 0, small_bytes = 0;
  double small_cpu = 0;
  std::vector<uint8_t> buf(128 * 1024);
  std::vector<uint64_t> stack{vol->root()};
  while (!stack.empty()) {
    uint64_t d = stack.back();
    stack.pop_back();
    std::vector<FsDirEntry> es;
    if (vol->readdir(d, &es) != 0) continue;
    for (auto& e : es) {
      if (e.name == "." || e.name == "..") continue;
      FsStat st;
      if (vol->stat(e.ino, &st) != 0) continue;
      if (st.is_dir()) { stack.push_back(e.ino); continue; }
      if ((st.mode & 0170000) != 0100000) continue;
      files++;
      uint64_t o = 0;
      for (;;) {
        int64_t r = vol->read(e.ino, o, buf.data(), buf.size());
        if (r <= 0) break;
        o += uint64_t(r);
      }
      bytes += o;
      if (st.size < (1u << 20)) { small_files++; small_bytes += o; }
    }
  }
  (void)small_cpu;
  double cpu = since(t);
  const double tot = cpu + simp->sim();
  std::printf("全ファイル読み: %llu ファイル (1 MiB 未満 %llu 個 / %.1f MB) 合計 %.1f MB  下位 %llu 回/%.1f MB 転送/%llu 往復  CPU %.3f s + I/O %.3f s  => %.2f MB/s\n",
              (unsigned long long)files, (unsigned long long)small_files, double(small_bytes) / 1e6, double(bytes) / 1e6, (unsigned long long)simp->reads(),
              double(simp->bytes()) / 1e6, (unsigned long long)simp->trips(), cpu, simp->sim(), tot > 0 ? double(bytes) / 1e6 / tot : 0.0);
  return 0;
}
#endif

// ---- プリミティブ
double mbps(size_t bytes, double s) { return s > 0 ? double(bytes) / 1e6 / s : 0; }

int do_prims() {
  using namespace crypto;
  const size_t N = 16u << 20;
  std::vector<uint8_t> buf(N), out(N);
  std::mt19937_64 rng(5);
  for (auto& b : buf) b = uint8_t(rng());
  uint8_t k1[32], k2[32];
  for (int i = 0; i < 32; i++) { k1[i] = uint8_t(i * 7 + 1); k2[i] = uint8_t(i * 5 + 3); }
  Aes a1, a2;
  a1.set_key(k1, 32); a2.set_key(k2, 32);
  auto t = Clock::now();
  for (size_t o = 0; o < N; o += 512) aes_xts_decrypt(a1, a2, o / 512, buf.data() + o, out.data() + o, 512);
  std::printf("AES-XTS-256 復号 (512 B セクタ)    %8.1f MB/s\n", mbps(N, since(t)));
  Aes b1;
  b1.set_key(k1, 16);
  uint8_t iv[16] = {1, 2, 3};
  t = Clock::now();
  for (size_t o = 0; o < N; o += 512) aes_cbc_decrypt(b1, iv, buf.data() + o, out.data() + o, 512);
  std::printf("AES-CBC-128 復号 (512 B セクタ)    %8.1f MB/s\n", mbps(N, since(t)));
#ifndef BENCH_BASE
  {  // 比較用に 1 バイトずつの表引き (従来の Btrfs の実装) も測る
    uint32_t tab[256];
    for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = (c & 1) ? (c >> 1) ^ 0x82F63B78u : c >> 1; tab[i] = c; }
    t = Clock::now();
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < N; i++) c = tab[(c ^ buf[i]) & 0xFF] ^ (c >> 8);
    g_sink = c;
    std::printf("CRC-32C 1 バイト表引き (従来)       %8.1f MB/s\n", mbps(N, since(t)));
    t = Clock::now();
    uint32_t r = crc32c_raw_sw(0xFFFFFFFFu, buf.data(), N);
    g_sink = r;
    std::printf("CRC-32C 8 バイト表引き              %8.1f MB/s\n", mbps(N, since(t)));
    t = Clock::now();
    r = crc32c_raw(0xFFFFFFFFu, buf.data(), N);
    g_sink = r;
    std::printf("CRC-32C 命令 (%s)                 %8.1f MB/s\n", crc32c_hw_active() ? "有" : "無", mbps(N, since(t)));
  }
  std::printf("AES 命令: %s\n", Aes::hw_supported() ? "使える (自己テスト合格)" : "使わない");
  {
    Aes s1, s2;
    s1.set_key_sw(k1, 32); s2.set_key_sw(k2, 32);
    t = Clock::now();
    for (size_t o = 0; o < N; o += 512) aes_xts_decrypt(s1, s2, o / 512, buf.data() + o, out.data() + o, 512);
    std::printf("AES-XTS-256 復号 (表引き強制)      %8.1f MB/s\n", mbps(N, since(t)));
  }
#endif
  uint8_t h[64];
  t = Clock::now();
  hash(Hash::Sha256, buf.data(), N, h);
#ifndef BENCH_BASE
  std::printf("SHA-256 (%s)                      %8.1f MB/s\n", sha256_hw_supported() ? "命令" : "表引き", mbps(N, since(t)));
  sha256_set_hw_enabled(false);
  t = Clock::now();
  hash(Hash::Sha256, buf.data(), N, h);
  std::printf("SHA-256 (表引き強制)               %8.1f MB/s\n", mbps(N, since(t)));
  sha256_set_hw_enabled(true);
#else
  std::printf("SHA-256                            %8.1f MB/s\n", mbps(N, since(t)));
#endif
  t = Clock::now();
  hash(Hash::Sha512, buf.data(), N, h);
  std::printf("SHA-512                            %8.1f MB/s\n", mbps(N, since(t)));
  t = Clock::now();
  uint8_t dk[64];
  pbkdf2(Hash::Sha256, k1, 16, k2, 16, 200000, dk, 32);
  double sec = since(t);
  std::printf("PBKDF2-HMAC-SHA256 200000 反復     %8.3f s (%.0f 反復/s)\n", sec, 200000 / sec);
  t = Clock::now();
  pbkdf2(Hash::Sha512, k1, 16, k2, 16, 100000, dk, 64);
  sec = since(t);
  std::printf("PBKDF2-HMAC-SHA512 100000 反復     %8.3f s (%.0f 反復/s)\n", sec, 100000 / sec);
  t = Clock::now();
  argon2(Argon2Type::ID, k1, 16, k2, 16, 2, 65536, 4, dk, 32, 1u << 20);
  std::printf("Argon2id 64 MiB t=2 p=4            %8.3f s\n", since(t));
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc >= 2 && std::string(argv[1]) == "prims") return do_prims();
  if (argc >= 2 && std::string(argv[1]) == "vol") return do_vol(argc, argv);
#ifdef USBMEM_BENCH_BTRFS
  if (argc >= 2 && std::string(argv[1]) == "fs") return do_fs(argc, argv);
#endif
  std::fprintf(stderr, "usage: %s prims | vol <image> <off> <len> <cred> [--lat-us N] [--bw MBPS] [--maxreq B] [--total MIB] [--files N] [--small-only]\n", argv[0]);
  return 2;
}
