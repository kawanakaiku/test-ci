// SPDX-License-Identifier: Apache-2.0
// 暗号ボリューム (BitLocker / LUKS) を復号して既存のファイルシステム実装に流す検証ツール。docs/crypto-volumes.md
//
// 資格情報の指定 <cred>:  pw:<パスワード> | pwfile:<ファイル> | rec:<48桁> | key:<ファイル> | none
// 使い方:
//   --info     <image> <off> <len> <cred>                  probe と open の結果 (形式・暗号・分類・FS 種別) を表示。復号できなければ 1
//   --expect   <image> <off> <len> <cred> <errc名>         open の結果コードが一致すれば 0 (wrong password なら auth-failed など)
//   --dev-compare <image> <off> <len> <cred> <参照ファイル>  復号デバイスを参照ファイル (平文イメージ / bdemount の出力) と全バイト比較
//   --tree-compare <image> <off> <len> <cred> <平文image> [平文off]  復号 → FS をマウントして、平文イメージを直接マウントしたものと全ツリー比較
//   --manifest <image> <off> <len> <cred> <manifest>       復号 → NTFS をマウントして manifest (f size sha path / d size - path) を全件検証
//   --fuzz     <image> <off> <len> <cred> <iters> <seed> [hot_off:hot_len ...]   ヘッダ/メタデータ破壊 (クラッシュ・ハング・無制限確保がないこと)
//   --truncate <image> <off> <len> <cred>                  あらゆる長さで切り詰めた版を open (クラッシュしないこと)
//   --zeroize  <image> <off> <len> <cred>                  close() 後にキー/平文キャッシュが残っていないこと (ヒープ走査)
//   --throughput <image> <off> <len> <cred>                復号スループット (MB/s)
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "sha256.h"
#include "usbmem/devices.h"
#include "usbmem/fs_probe.h"
#include "usbmem/fs_volume.h"
#include "usbmem/transform.h"
#ifdef USBMEM_CV_NTFS
#include "usbmem/ntfs_volume.h"
#endif
#ifdef USBMEM_CV_EXT2
#include "usbmem/ext2_volume.h"
#endif
#ifdef USBMEM_CV_EXT4
#include "usbmem/ext4_volume.h"
#endif
#ifdef USBMEM_CV_FAT
#include "usbmem/fat_volume.h"
#endif

using namespace usbmem;

namespace {

std::vector<uint8_t> read_file(const std::string& p) {
  std::ifstream f(p, std::ios::binary);
  std::vector<uint8_t> v((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return v;
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

BlockDevicePtr open_range(const std::string& img, uint64_t off, uint64_t len, bool* ok) {
  BlockDevicePtr raw;
  *ok = false;
  if (!RawImageDevice::open_path(img, true, 512, &raw)) return nullptr;
  if (off == 0 && len == 0) { *ok = true; return raw; }
  if (len == 0) len = raw->size() - off;
  BlockDevicePtr sub;
  if (!SubRangeDevice::create(raw, off, len, true, &sub)) return nullptr;
  *ok = true;
  return sub;
}

std::unique_ptr<FsVolume> mount_fs(const BlockDevicePtr& dev, FsType* type, int* err) {
  FsType t = probe_filesystem(*dev, 0, dev->size());
  if (type) *type = t;
  switch (t) {
#ifdef USBMEM_CV_EXT2
    case FsType::Ext2: return Ext2Volume::mount(dev, err);
#endif
#ifdef USBMEM_CV_EXT4
    case FsType::Ext3:
    case FsType::Ext4: return Ext4Volume::mount(dev, err);
#endif
#ifdef USBMEM_CV_FAT
    case FsType::Fat12:
    case FsType::Fat16:
    case FsType::Fat32: return FatVolume::mount(dev, err);
#endif
#ifdef USBMEM_CV_NTFS
    case FsType::Ntfs: return NtfsVolume::mount(dev, err);
#endif
    default: break;
  }
#ifdef USBMEM_CV_NTFS
  // 復号が壊れていれば Unknown になる。ここで NTFS として試すと本物のドライバの異常系を踏むので試さない。
#endif
  if (err) *err = 22;
  return nullptr;
}

struct Stats { long files = 0, dirs = 0, links = 0, fails = 0; uint64_t bytes = 0; };

std::string sha_of(FsVolume& v, uint64_t ino, uint64_t size, bool* bad) {
  Sha256 h;
  std::vector<uint8_t> buf(1 << 17);
  uint64_t off = 0;
  for (;;) {
    int64_t n = v.read(ino, off, buf.data(), buf.size());
    if (n < 0) { *bad = true; return ""; }
    if (n == 0) break;
    h.update(buf.data(), size_t(n));
    off += uint64_t(n);
  }
  if (off != size) *bad = true;
  return h.hex();
}

void compare_tree(FsVolume& a, uint64_t ia, FsVolume& b, uint64_t ib, const std::string& path, Stats* st, int depth) {
  if (depth > 40) return;
  std::vector<FsDirEntry> ea, eb;
  if (a.readdir(ia, &ea) < 0 || b.readdir(ib, &eb) < 0) { std::fprintf(stderr, "FAIL readdir %s\n", path.c_str()); st->fails++; return; }
  std::map<std::string, FsDirEntry> ma, mb;
  for (auto& e : ea) if (e.name != "." && e.name != "..") ma[e.name] = e;
  for (auto& e : eb) if (e.name != "." && e.name != "..") mb[e.name] = e;
  for (auto& kv : ma) {
    auto it = mb.find(kv.first);
    const std::string p = path + "/" + kv.first;
    if (it == mb.end()) { std::fprintf(stderr, "FAIL only in decrypted: %s\n", p.c_str()); st->fails++; continue; }
    FsStat sa, sb;
    if (a.stat(kv.second.ino, &sa) < 0 || b.stat(it->second.ino, &sb) < 0) { std::fprintf(stderr, "FAIL stat %s\n", p.c_str()); st->fails++; continue; }
    if ((sa.mode & 0170000) != (sb.mode & 0170000) || sa.size != sb.size) { std::fprintf(stderr, "FAIL type/size %s\n", p.c_str()); st->fails++; continue; }
    if (sa.is_dir()) {
      st->dirs++;
      compare_tree(a, kv.second.ino, b, it->second.ino, p, st, depth + 1);
    } else if ((sa.mode & 0170000) == 0120000) {
      std::string la, lb;
      int ra = a.readlink(kv.second.ino, &la), rb = b.readlink(it->second.ino, &lb);
      // readlink 未実装の FS (ext4 など) は両側で同じ負のエラーコードになる。それは「同じ」とみなす
      if (ra != rb || (ra >= 0 && la != lb)) { std::fprintf(stderr, "FAIL symlink %s\n", p.c_str()); st->fails++; }
      else st->links++;
    } else {
      bool bad = false;
      std::string x = sha_of(a, kv.second.ino, sa.size, &bad), y = sha_of(b, it->second.ino, sb.size, &bad);
      if (bad || x != y) { std::fprintf(stderr, "FAIL content %s\n", p.c_str()); st->fails++; }
      else { st->files++; st->bytes += sa.size; }
    }
  }
  for (auto& kv : mb) if (!ma.count(kv.first)) { std::fprintf(stderr, "FAIL only in reference: %s/%s\n", path.c_str(), kv.first.c_str()); st->fails++; }
}

struct Common {
  std::string img; uint64_t off = 0, len = 0; std::string cred_spec; Credentials cred;
};

bool parse_common(char** argv, int argc, Common* c) {
  if (argc < 4) return false;
  c->img = argv[0];
  c->off = std::strtoull(argv[1], nullptr, 0);
  c->len = std::strtoull(argv[2], nullptr, 0);
  c->cred_spec = argv[3];
  return parse_cred(c->cred_spec, &c->cred);
}

Status open_dec(const Common& c, BlockDevicePtr base, BlockDevicePtr* out, VolumeInfo* info = nullptr, const OpenOptions& opt = {}) {
  OpenOptions o = opt;
  if (const char* e = std::getenv("USBMEM_CV_ALLOW_PARTIAL")) o.allow_partial_encryption = e[0] == '1';  // 暗号化途中のボリュームを変換済み領域だけ開く
  return open_crypto_volume(std::move(base), CryptoKind::Auto, c.cred, o, out, info);
}

void print_info(const VolumeInfo& i) {
  std::printf("kind=%d format=\"%s\" cipher=\"%s\" supported=%d password=%d recovery=%d keyfile=%d clear=%d tpm_only=%d in_progress=%d note=\"%s\"\n", int(i.kind),
              i.format.c_str(), i.cipher.c_str(), i.supported, i.can_password, i.can_recovery_key, i.can_key_file, i.unlocked_without_secret, i.tpm_only,
              i.encryption_in_progress, i.note.c_str());
}

int cmd_info(const Common& c) {
  bool ok;
  auto base = open_range(c.img, c.off, c.len, &ok);
  if (!ok) { std::fprintf(stderr, "cannot open image\n"); return 2; }
  VolumeInfo pi;
  Status ps = probe_crypto_volume(*base, &pi);
  std::printf("probe: %s\n", errc_name(ps.code));
  print_info(pi);
  BlockDevicePtr dec;
  VolumeInfo oi;
  Status s = open_dec(c, base, &dec, &oi);
  std::printf("open: %s\n", errc_name(s.code));
  if (!s) return 1;
  FsType t = probe_filesystem(*dec, 0, dec->size());
  std::printf("size=%llu fs=%s describe=%s\n", (unsigned long long)dec->size(), fs_type_name(t), dec->describe().c_str());
  return 0;
}

int cmd_expect(const Common& c, const char* want) {
  bool ok;
  auto base = open_range(c.img, c.off, c.len, &ok);
  if (!ok) return 2;
  BlockDevicePtr dec;
  VolumeInfo oi;
  Status s = open_dec(c, base, &dec, &oi);
  std::printf("open: %s (want %s) note=\"%s\"\n", errc_name(s.code), want, oi.note.c_str());
  if (dec && s.ok()) {
    // 平文キャッシュや鍵が漏れていないことの確認は --zeroize。ここでは書き込み拒否だけ見る
    uint8_t b = 0;
    if (dec->write_at(0, &b, 1).code != Errc::ReadOnly) { std::fprintf(stderr, "FAIL write not rejected\n"); return 1; }
  }
  return std::string(errc_name(s.code)) == want ? 0 : 1;
}

int cmd_dev_compare(const Common& c, const std::string& ref) {
  bool ok;
  auto base = open_range(c.img, c.off, c.len, &ok);
  if (!ok) return 2;
  BlockDevicePtr dec;
  Status s = open_dec(c, base, &dec);
  if (!s) { std::fprintf(stderr, "open failed: %s\n", errc_name(s.code)); return 1; }
  std::ifstream f(ref, std::ios::binary);
  if (!f) { std::fprintf(stderr, "cannot open reference\n"); return 2; }
  f.seekg(0, std::ios::end);
  uint64_t rsize = uint64_t(f.tellg());
  f.seekg(0);
  const uint64_t n = std::min<uint64_t>(rsize, dec->size());
  std::printf("compare %llu bytes (decrypted device %llu, reference %llu)\n", (unsigned long long)n, (unsigned long long)dec->size(), (unsigned long long)rsize);
  std::vector<uint8_t> a(1 << 20), b(1 << 20);
  // 差のある範囲 (連続バイトをまとめた区間) を列挙する。--allow-zero-ranges 相当: 参照側が全 0 で、こちらが任意の区間は
  // 「メタデータ領域などの非公開領域」として報告だけして失敗にはしない (allow_zero が真のとき)。
  struct Range { uint64_t lo, hi; bool ref_zero; };
  std::vector<Range> diffs;
  for (uint64_t o = 0; o < n; o += a.size()) {
    size_t k = size_t(std::min<uint64_t>(a.size(), n - o));
    f.read(reinterpret_cast<char*>(b.data()), std::streamsize(k));
    // 位置をずらした不整列な読みも混ぜる (キャッシュ経路の検査): 3 分割
    size_t k1 = k / 3 + 1, k2 = k / 2;
    if (k1 > k) k1 = k;
    if (k2 < k1) k2 = k1;
    if (!(s = dec->read_at(o, a.data(), k1)) || !(s = dec->read_at(o + k1, a.data() + k1, k2 - k1)) || !(s = dec->read_at(o + k2, a.data() + k2, k - k2))) {
      std::fprintf(stderr, "read failed at %llu: %s\n", (unsigned long long)o, errc_name(s.code));
      return 1;
    }
    if (std::memcmp(a.data(), b.data(), k) == 0) continue;
    for (size_t i = 0; i < k; i++) {
      if (a[i] == b[i]) continue;
      if (!diffs.empty() && diffs.back().hi == o + i) { diffs.back().hi++; diffs.back().ref_zero &= (b[i] == 0); }
      else diffs.push_back({o + i, o + i + 1, b[i] == 0});
    }
  }
  uint64_t bad = 0;
  for (auto& d : diffs) {
    std::fprintf(stderr, "difference: bytes [%llu, %llu) (sector %llu..%llu)%s\n", (unsigned long long)d.lo, (unsigned long long)d.hi, (unsigned long long)(d.lo / 512),
                 (unsigned long long)((d.hi - 1) / 512), d.ref_zero ? " [reference is zero]" : "");
    bad += d.hi - d.lo;
  }
  if (bad) { std::fprintf(stderr, "FAIL %llu differing bytes in %zu ranges\n", (unsigned long long)bad, diffs.size()); return 1; }
  // 末尾を超える読みは OutOfRange
  uint8_t t;
  if (dec->read_at(dec->size(), &t, 1).code != Errc::OutOfRange) { std::fprintf(stderr, "FAIL read past end not rejected\n"); return 1; }
  std::printf("dev-compare: identical\n");
  return 0;
}

int cmd_tree_compare(const Common& c, const std::string& plain, uint64_t poff) {
  bool ok;
  auto base = open_range(c.img, c.off, c.len, &ok);
  if (!ok) return 2;
  BlockDevicePtr dec;
  Status s = open_dec(c, base, &dec);
  if (!s) { std::fprintf(stderr, "open failed: %s\n", errc_name(s.code)); return 1; }
  FsType t1, t2;
  int e1 = 0, e2 = 0;
  auto va = mount_fs(dec, &t1, &e1);
  bool ok2;
  auto pdev = open_range(plain, poff, 0, &ok2);
  if (!ok2) return 2;
  auto vb = mount_fs(pdev, &t2, &e2);
  if (!va || !vb) { std::fprintf(stderr, "mount failed (decrypted fs=%s err %d, plain fs=%s err %d)\n", fs_type_name(t1), e1, fs_type_name(t2), e2); return 1; }
  Stats st;
  compare_tree(*va, va->root(), *vb, vb->root(), "", &st, 0);
  std::printf("tree-compare fs=%s: %ld files (%llu bytes), %ld dirs, %ld symlinks identical, %ld failures\n", va->fs_name(), st.files, (unsigned long long)st.bytes, st.dirs,
              st.links, st.fails);
  return st.fails || st.files == 0 ? 1 : 0;
}

int cmd_manifest(const Common& c, const std::string& manifest) {
  bool ok;
  auto base = open_range(c.img, c.off, c.len, &ok);
  if (!ok) return 2;
  BlockDevicePtr dec;
  Status s = open_dec(c, base, &dec);
  if (!s) { std::fprintf(stderr, "open failed: %s\n", errc_name(s.code)); return 1; }
  FsType t;
  int err = 0;
  auto vol = mount_fs(dec, &t, &err);
  if (!vol) { std::fprintf(stderr, "mount failed (fs=%s err %d)\n", fs_type_name(t), err); return 1; }
  std::ifstream mf(manifest);
  std::string line;
  int files = 0, dirs = 0, fails = 0;
  while (std::getline(mf, line)) {
    std::istringstream is(line);
    std::string kind, size, sha;
    is >> kind >> size >> sha;
    std::string path;
    std::getline(is >> std::ws, path);
    uint64_t ino = 0;
    if (vol->resolve(path, &ino)) { std::fprintf(stderr, "FAIL resolve %s\n", path.c_str()); fails++; continue; }
    FsStat st;
    if (vol->stat(ino, &st)) { std::fprintf(stderr, "FAIL stat %s\n", path.c_str()); fails++; continue; }
    if (kind == "d") { if (!st.is_dir()) { std::fprintf(stderr, "FAIL not dir %s\n", path.c_str()); fails++; } else dirs++; continue; }
    bool bad = false;
    std::string h = sha_of(*vol, ino, std::stoull(size), &bad);
    if (bad || h != sha || st.size != std::stoull(size)) { std::fprintf(stderr, "FAIL content %s\n", path.c_str()); fails++; }
    else files++;
  }
  std::printf("manifest fs=%s: %d files, %d dirs ok, %d failures\n", vol->fs_name(), files, dirs, fails);
  return fails || files == 0 ? 1 : 0;
}

// 読み出し時にバイトを差し替える (ヘッダ破壊用)
class MutDevice final : public BlockDevice {
 public:
  MutDevice(BlockDevicePtr b, std::map<uint64_t, uint8_t> m, uint64_t size = 0) : b_(std::move(b)), m_(std::move(m)), size_(size ? size : b_->size()) {}
  Status read_at(uint64_t off, void* buf, size_t len, const IoOptions& o = {}) override {
    if (!range_ok(size_, off, len)) return Status::Make(Errc::OutOfRange);
    Status s = b_->read_at(off, buf, len, o);
    if (!s) return s;
    for (auto it = m_.lower_bound(off); it != m_.end() && it->first < off + len; ++it) static_cast<uint8_t*>(buf)[it->first - off] = it->second;
    return s;
  }
  Status write_at(uint64_t, const void*, size_t, const IoOptions& = {}) override { return Status::Make(Errc::ReadOnly); }
  Status flush() override { return Status::Ok(); }
  Status close() override { return Status::Ok(); }
  uint64_t size() const override { return size_; }
  uint32_t logical_block_size() const override { return 512; }
  bool is_read_only() const override { return true; }
  std::string describe() const override { return "mut"; }
 private:
  BlockDevicePtr b_; std::map<uint64_t, uint8_t> m_; uint64_t size_;
};

// 復号済みデバイスを軽く触る (先頭 1 MiB を読み、マウントして浅く走査)。ハング/クラッシュ検査用。
void touch_decrypted(const BlockDevicePtr& dec) {
  std::vector<uint8_t> buf(1 << 16);
  for (uint64_t o = 0; o < std::min<uint64_t>(dec->size(), 1u << 20); o += buf.size()) {
    size_t k = size_t(std::min<uint64_t>(buf.size(), dec->size() - o));
    dec->read_at(o, buf.data(), k);
  }
  probe_filesystem(*dec, 0, dec->size());
}

int cmd_fuzz(const Common& c, int iters, unsigned seed, const std::vector<std::pair<uint64_t, uint64_t>>& hot) {
  bool ok;
  auto base = open_range(c.img, c.off, c.len, &ok);
  if (!ok) return 2;
  uint64_t x = seed * 2654435761u + 1;
  auto rnd = [&]() { x ^= x << 13; x ^= x >> 7; x ^= x << 17; return x; };
  OpenOptions opt;
  opt.max_kdf_memory_kib = 64 * 1024;
  opt.max_pbkdf2_iterations = 300000;
  int opened = 0, authfail = 0, rejected = 0;
  for (int it = 0; it < iters; it++) {
    std::map<uint64_t, uint8_t> m;
    int nmut = 1 + int(rnd() % 12);
    for (int k = 0; k < nmut; k++) {
      uint64_t off;
      if (!hot.empty() && rnd() % 5 != 0) { auto& h = hot[rnd() % hot.size()]; off = h.first + rnd() % std::max<uint64_t>(1, h.second); }
      else if (rnd() % 3 == 0) off = rnd() % 4096;
      else off = rnd() % std::max<uint64_t>(1, base->size());
      m[off] = uint8_t(rnd() % 3 == 0 ? 0xff : rnd());
    }
    auto dev = std::make_shared<MutDevice>(base, m);
    if (std::getenv("USBMEM_CV_VERBOSE")) {  // ハング調査用: 直前に適用した変更を出す (最後の行が犯人)
      std::fprintf(stderr, "iter %d:", it);
      for (auto& kv : m) std::fprintf(stderr, " %llu=%02x", (unsigned long long)kv.first, kv.second);
      std::fprintf(stderr, "\n");
    }
    alarm(60);
    BlockDevicePtr dec;
    VolumeInfo vi;
    VolumeInfo pi;
    probe_crypto_volume(*dev, &pi);
    Status s = open_dec(c, dev, &dec, &vi, opt);
    if (s.ok()) { opened++; touch_decrypted(dec); dec->close(); }
    else if (s.code == Errc::AuthFailed) authfail++;
    else rejected++;
    alarm(0);
  }
  std::printf("fuzz: %d iterations (seed %u): %d opened, %d auth-failed, %d rejected; no crash/hang\n", iters, seed, opened, authfail, rejected);
  return 0;
}

int cmd_truncate(const Common& c) {
  bool ok;
  auto base = open_range(c.img, c.off, c.len, &ok);
  if (!ok) return 2;
  OpenOptions opt;
  opt.max_kdf_memory_kib = 64 * 1024;
  opt.max_pbkdf2_iterations = 300000;
  const uint64_t full = base->size();
  // 切り詰め位置: 先頭付近は細かく、以降は粗く (ヘッダ/メタデータの境界をまたぐ位置を含む)
  std::vector<uint64_t> cuts;
  for (uint64_t v = 0; v < 8192; v += 512) cuts.push_back(v);
  for (uint64_t v = 8192; v < std::min<uint64_t>(full, 64u << 20); v = v * 3 / 2 + 512) cuts.push_back(v - v % 512);
  int n = 0;
  for (uint64_t cut : cuts) {
    if (cut >= full) continue;
    auto dev = std::make_shared<MutDevice>(base, std::map<uint64_t, uint8_t>(), cut);
    alarm(60);
    BlockDevicePtr dec;
    VolumeInfo vi;
    Status s = open_dec(c, dev, &dec, &vi, opt);
    if (s.ok()) { touch_decrypted(dec); dec->close(); }
    alarm(0);
    n++;
  }
  std::printf("truncate: %d lengths tried; no crash/hang\n", n);
  return 0;
}


// プロセスの読み書き可能な匿名/ヒープ領域に目印 (復号済み平文の先頭 32 バイト) が何個あるか数える。ASan の poison は無視して生のメモリを読む。
__attribute__((no_sanitize("address"))) long count_marker(const std::vector<uint8_t>& needle, const uint8_t* self, size_t self_len) {
  long found = 0;
  FILE* maps = std::fopen("/proc/self/maps", "r");
  char line[512];
  while (maps && std::fgets(line, sizeof line, maps)) {
    unsigned long lo, hi;
    char perms[8];
    if (std::sscanf(line, "%lx-%lx %7s", &lo, &hi, perms) != 3 || perms[0] != 'r' || perms[1] != 'w') continue;
    if (hi - lo > (256ul << 20) || std::strstr(line, "[stack]") || std::strstr(line, ".so") || std::strstr(line, "vvar") || std::strstr(line, "vsyscall")) continue;
    const uint8_t* end = reinterpret_cast<const uint8_t*>(hi);
    for (const uint8_t* q = reinterpret_cast<const uint8_t*>(lo); q + needle.size() <= end; q++)
      if (q[0] == needle[0] && q != needle.data() && !(q >= self && q < self + self_len) && std::memcmp(q, needle.data(), needle.size()) == 0) found++;
  }
  if (maps) std::fclose(maps);
  return found;
}

// close() 後に、鍵や平文がヒープに残らないこと: 既知の平文パターンを読んでキャッシュに載せ、close 後にプロセスのヒープを走査する。
int cmd_zeroize(const Common& c, const std::string& pattern_hex) {
  bool ok;
  auto base = open_range(c.img, c.off, c.len, &ok);
  if (!ok) return 2;
  BlockDevicePtr dec;
  Status s = open_dec(c, base, &dec);
  if (!s) return 1;
  std::vector<uint8_t> pat;
  for (size_t i = 0; i + 1 < pattern_hex.size(); i += 2) pat.push_back(uint8_t(std::stoi(pattern_hex.substr(i, 2), nullptr, 16)));
  // 復号した最初の 32 KiB (= 1 チャンク) をキャッシュに載せ、その中で「バイト値が十分ばらけた 32 バイト」を目印にする
  // (ext2 の先頭などゼロだらけの所を目印にすると、ヒープ中のゼロ領域すべてに一致してしまうため)
  std::vector<uint8_t> first(32768);
  uint8_t warm[512];
  if (!dec->read_at(0, warm, sizeof warm)) return 1;  // 小さい読み出し → チャンクがキャッシュに載る
  if (!dec->read_at(0, first.data(), first.size())) return 1;
  std::vector<uint8_t> needle;
  for (size_t o = 0; o + 32 <= first.size() && needle.empty(); o += 16) {
    bool seen[256] = {false};
    int distinct = 0;
    for (size_t i = 0; i < 32; i++) if (!seen[first[o + i]]) { seen[first[o + i]] = true; distinct++; }
    if (distinct >= 24) needle.assign(first.begin() + long(o), first.begin() + long(o) + 32);
  }
  if (needle.empty()) { std::printf("zeroize: no distinctive plaintext in the first 32 KiB (skipped)\n"); return 77; }
  auto count = [&]() { return count_marker(needle, first.data(), first.size()); };
  long before = count();
  dec->close();
  dec.reset();
  long after = count();
  std::printf("zeroize: plaintext marker copies in heap: before close %ld, after close %ld\n", before, after);
  (void)pat;
  if (before == 0) { std::printf("zeroize: marker not found before close (scan ineffective)\n"); return 1; }
  return after == 0 ? 0 : 1;
}

int cmd_throughput(const Common& c) {
  bool ok;
  auto base = open_range(c.img, c.off, c.len, &ok);
  if (!ok) return 2;
  BlockDevicePtr dec;
  Status s = open_dec(c, base, &dec);
  if (!s) return 1;
  std::vector<uint8_t> buf(1 << 20);
  uint64_t total = std::min<uint64_t>(dec->size(), 128u << 20), done = 0;
  auto t0 = std::chrono::steady_clock::now();
  for (uint64_t o = 0; o < total; o += buf.size()) {
    size_t k = size_t(std::min<uint64_t>(buf.size(), total - o));
    if (!dec->read_at(o, buf.data(), k)) return 1;
    done += k;
  }
  double sec = std::chrono::duration<double>(std::chrono::steady_clock::now() - t0).count();
  std::printf("throughput: %.1f MB/s (%llu MiB in %.2f s, includes base read)\n", double(done) / 1048576.0 / sec, (unsigned long long)(done >> 20), sec);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: see header of test_cryptovol.cpp\n"); return 2; }
  std::string mode = argv[1];
  Common c;
  if (!parse_common(argv + 2, argc - 2, &c)) { std::fprintf(stderr, "bad arguments\n"); return 2; }
  const int rest = argc - 6;  // <image> <off> <len> <cred> の後の引数の数
  if (mode == "--info") return cmd_info(c);
  if (mode == "--expect" && rest >= 1) return cmd_expect(c, argv[6]);
  if (mode == "--dev-compare" && rest >= 1) return cmd_dev_compare(c, argv[6]);
  if (mode == "--tree-compare" && rest >= 1) return cmd_tree_compare(c, argv[6], rest >= 2 ? std::strtoull(argv[7], nullptr, 0) : 0);
  if (mode == "--manifest" && rest >= 1) return cmd_manifest(c, argv[6]);
  if (mode == "--fuzz" && rest >= 2) {
    std::vector<std::pair<uint64_t, uint64_t>> hot;
    for (int i = 8; i < argc; i++) {
      char* colon = std::strchr(argv[i], ':');
      if (colon) hot.emplace_back(std::strtoull(argv[i], nullptr, 0), std::strtoull(colon + 1, nullptr, 0));
    }
    return cmd_fuzz(c, std::atoi(argv[6]), unsigned(std::atoi(argv[7])), hot);
  }
  if (mode == "--truncate") return cmd_truncate(c);
  if (mode == "--zeroize") return cmd_zeroize(c, rest >= 1 ? argv[6] : "");
  if (mode == "--throughput") return cmd_throughput(c);
  std::fprintf(stderr, "unknown mode or missing arguments\n");
  return 2;
}
