// SPDX-License-Identifier: Apache-2.0
// 「ディスク全体」の構成での暗号ボリュームの検証ツール。docs/crypto-volumes.md 6.5
//
//   ディスクイメージ → パーティション走査 (read_partition_table) → 各ノード (パーティション / ディスク全体 / 復号済みデバイス) を調べる
//     - 暗号ボリューム (probe_crypto_volume) なら資格情報を順に試して open_crypto_volume し、復号済みデバイスを再び走査 (中にパーティション表があっても効く)
//     - FS なら probe_filesystem → マウント → manifest と全件照合 (欠け・内容違い・manifest に無い余分なエントリがないこと)
//   アプリ (android の RealUsbCommon.mountPartition / JNI) と同じ順序: パーティション → 暗号の判定 → 解錠 → 中の FS。
//
// 使い方:
//   --scan <image> [--cred [KEY=]<cred> ...] [--cut N] [--clip KEY=N ...] [--poke OFF=HEX ...] [--lenient] [KEY=<期待> ...]
//
//   <cred>  pw:<パスワード> | pwfile:<ファイル> | rec:<48桁> | key:<ファイル>。KEY= を付けるとそのノードだけに使う。付けなければ全ノードで順に試す
//           (AuthFailed なら次の資格情報へ。どれも合わなければ auth-failed)。
//   --cut N          イメージを先頭 N バイトで切り詰めたものとして扱う (ディスクの途中までの欠け)
//   --clip KEY=N     パーティション KEY (例 d.p3) を、表に書かれた長さに関わらず先頭 N バイトまでの長さにする (パーティション表は無傷のまま、そのパーティションだけ途中までしかない状態)
//   --poke OFF=HEX   読み出し時に OFF から HEX のバイト列へ差し替える (ヘッダ破壊。ファイルは書き換えない)
//   --lenient        期待に無いノードが見つかっても失敗にしない (既定は、見つかったノードはすべて期待を書く必要がある)
//
//   ノードの名前 (KEY): ディスク全体 = d、パーティション = <親>.p<番号>、暗号ボリュームの復号済みデバイス = <親>.dec。
//     例: d.p2 (GPT の 2 番)、d.p2.dec (その復号後 = FS)、d.dec.p1 (ディスク全体が暗号で、中の GPT の 1 番)。`<KEY>#table` はパーティション表の種別 (任意)。
//   期待 (<KEY>=...):
//     verified:<manifest>   FS をマウントして manifest と全件一致 (余分なエントリも不一致)     [FS のノード]
//     unlocked              解錠できた                                                      [暗号のノード]
//     auth-failed / corrupt / unsupported / io ...  open_crypto_volume の結果コード名          [暗号のノード]
//     no-credentials        暗号ボリュームだが使える資格情報がない                            [暗号のノード]
//     unknown-fs            暗号でも既知の FS でもない
//     mount-failed          FS を判定できたがマウントできない
//     gpt / gpt-backup / mbr / none  (KEY#table) パーティション表の種別 (gpt-backup = 先頭が壊れて予備から復元)
//     safe                  無い (見つからない) か、何であっても「誤った内容を見せる」(mismatch) でなければ可 (切り詰め・破損の試験用)
//   FS の結果は verified / partial (読めないエントリがあるが誤った内容はない) / mismatch (内容違い・余分なエントリ)。
// 終了コード: 期待どおりなら 0。クラッシュ・ハングは (alarm で) 異常終了になる。
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "sha256.h"
#include "usbmem/devices.h"
#include "usbmem/fs_probe.h"
#include "usbmem/fs_volume.h"
#include "usbmem/partition.h"
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
  return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

bool parse_cred(const std::string& spec, Credentials* c) {
  auto set = [](crypto::Secret& s, const std::string& v) { s.assign(reinterpret_cast<const uint8_t*>(v.data()), v.size()); };
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

// 読み出し時にバイトを差し替える / 先頭 size バイトで切り詰める
class MutDevice final : public BlockDevice {
 public:
  MutDevice(BlockDevicePtr b, std::map<uint64_t, uint8_t> m, uint64_t size) : b_(std::move(b)), m_(std::move(m)), size_(size ? std::min(size, b_->size()) : b_->size()) {}
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
  uint32_t logical_block_size() const override { return b_->logical_block_size(); }
  bool is_read_only() const override { return true; }
  std::string describe() const override { return "mut"; }

 private:
  BlockDevicePtr b_;
  std::map<uint64_t, uint8_t> m_;
  uint64_t size_;
};

std::unique_ptr<FsVolume> mount_fs(const BlockDevicePtr& dev, FsType t, int* err) {
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
  if (err) *err = 22;
  return nullptr;
}

struct ManEntry { char kind; uint64_t size; std::string sha, path; };

std::vector<ManEntry> load_manifest(const std::string& file, bool* ok) {
  std::vector<ManEntry> v;
  std::ifstream mf(file);
  *ok = bool(mf);
  std::string line;
  while (std::getline(mf, line)) {
    std::istringstream is(line);
    std::string kind, size, sha, path;
    is >> kind >> size >> sha;
    std::getline(is >> std::ws, path);
    if (kind.empty() || path.empty()) continue;
    v.push_back({kind[0], std::strtoull(size.c_str(), nullptr, 10), sha, path});
  }
  return v;
}

enum class Rd { Ok, Error, Short };

// 全内容の SHA-256。読み取りエラー (n < 0) は Error、サイズ不足で終わったら Short。
Rd sha_of(FsVolume& v, uint64_t ino, uint64_t size, std::string* hex) {
  Sha256 h;
  std::vector<uint8_t> buf(1 << 17);
  uint64_t off = 0;
  for (;;) {
    int64_t n = v.read(ino, off, buf.data(), buf.size());
    if (n < 0) return Rd::Error;
    if (n == 0) break;
    h.update(buf.data(), size_t(n));
    off += uint64_t(n);
  }
  if (off != size) return Rd::Short;
  *hex = h.hex();
  return Rd::Ok;
}

struct Verify { long files = 0, dirs = 0, mismatches = 0, errors = 0, extras = 0; uint64_t bytes = 0; };

void walk_extras(FsVolume& v, uint64_t dir, const std::string& path, const std::set<std::string>& known, Verify* r, int depth) {
  if (depth > 40) return;
  std::vector<FsDirEntry> ents;
  if (v.readdir(dir, &ents) < 0) { r->errors++; return; }
  for (auto& e : ents) {
    if (e.name == "." || e.name == "..") continue;
    if (path.empty() && (e.name == "lost+found" || e.name == "System Volume Information")) continue;  // mke2fs / Windows が作るもの
    std::string p = path + "/" + e.name;
    if (!known.count(p)) { std::fprintf(stderr, "  extra entry not in manifest: %s\n", p.c_str()); r->extras++; continue; }
    FsStat st;
    if (v.stat(e.ino, &st) == 0 && st.is_dir()) walk_extras(v, e.ino, p, known, r, depth + 1);
  }
}

Verify verify_manifest(FsVolume& v, const std::vector<ManEntry>& man) {
  Verify r;
  std::set<std::string> known;
  for (auto& m : man) known.insert("/" + m.path);
  for (auto& m : man) {
    uint64_t ino = 0;
    FsStat st;
    if (v.resolve("/" + m.path, &ino) || v.stat(ino, &st)) { std::fprintf(stderr, "  missing/unreadable: %s\n", m.path.c_str()); r.errors++; continue; }
    if (m.kind == 'd') {
      if (!st.is_dir()) { std::fprintf(stderr, "  not a directory: %s\n", m.path.c_str()); r.mismatches++; }
      else r.dirs++;
      continue;
    }
    if (st.is_dir() || st.size != m.size) { std::fprintf(stderr, "  type/size differs: %s (size %llu, manifest %llu)\n", m.path.c_str(), (unsigned long long)st.size, (unsigned long long)m.size); r.mismatches++; continue; }
    std::string hex;
    Rd rd = sha_of(v, ino, m.size, &hex);
    if (rd == Rd::Ok && hex == m.sha) { r.files++; r.bytes += m.size; }
    else if (rd == Rd::Ok) { std::fprintf(stderr, "  CONTENT DIFFERS: %s\n", m.path.c_str()); r.mismatches++; }
    else { std::fprintf(stderr, "  read %s: %s\n", m.path.c_str(), rd == Rd::Error ? "error" : "short"); r.errors++; }
  }
  walk_extras(v, v.root(), "", known, &r, 0);
  return r;
}

struct Expect { std::string what, manifest; bool seen = false; };

struct Ctx {
  using CredPtr = std::shared_ptr<Credentials>;
  std::vector<CredPtr> global_creds;
  std::map<std::string, std::vector<CredPtr>> bound;
  std::map<std::string, Expect> expects;
  std::map<std::string, uint64_t> clip;
  bool lenient = false;
  int failures = 0;
  int nodes = 0, verified = 0, unlocked = 0;
};

void fail(Ctx& c, const char* fmt, const std::string& a, const std::string& b = "", const std::string& d = "") {
  c.failures++;
  std::fprintf(stderr, "FAIL ");
  std::fprintf(stderr, fmt, a.c_str(), b.c_str(), d.c_str());
  std::fputc('\n', stderr);
}

// ノードの結果を記録して、期待と突き合わせる
void report(Ctx& c, const std::string& key, const std::string& outcome, const std::string& detail = "") {
  const bool is_table = key.find('#') != std::string::npos;
  if (!is_table) c.nodes++;
  std::printf("node %-14s %s%s%s\n", key.c_str(), outcome.c_str(), detail.empty() ? "" : "  ", detail.c_str());
  auto it = c.expects.find(key);
  if (it == c.expects.end()) {
    if (!is_table && !c.lenient) fail(c, "node %s: unexpected (outcome %s)", key, outcome);
    return;
  }
  Expect& e = it->second;
  e.seen = true;
  if (e.what == "safe") {
    if (outcome == "mismatch") fail(c, "node %s: expected safe, got %s", key, outcome);
  } else if (outcome != e.what) {
    fail(c, "node %s: expected %s, got %s", key, e.what, outcome);
  }
}

void walk(Ctx& c, const BlockDevicePtr& dev, const std::string& key, int depth);

// 1 つの「ボリューム候補」(パーティション / ディスク全体 / 復号済みデバイス) を調べる
void handle_volume(Ctx& c, const BlockDevicePtr& dev, const std::string& key, int depth) {
  VolumeInfo vi;
  Status ps = probe_crypto_volume(*dev, &vi);
  const bool is_crypto = ps.ok() || vi.kind != CryptoKind::Auto;
  if (is_crypto) {
    std::vector<Ctx::CredPtr> creds;
    auto b = c.bound.find(key);
    if (b != c.bound.end()) for (auto& x : b->second) creds.push_back(x);
    for (auto& x : c.global_creds) creds.push_back(x);
    if (creds.empty()) { report(c, key, "no-credentials", vi.format); return; }
    BlockDevicePtr dec;
    Status last = Status::Make(Errc::AuthFailed);
    for (auto& cr : creds) {
      VolumeInfo oi;
      last = open_crypto_volume(dev, CryptoKind::Auto, *cr, OpenOptions{}, &dec, &oi);
      if (last.ok() || last.code != Errc::AuthFailed) break;
    }
    if (!last.ok()) { report(c, key, errc_name(last.code), vi.format); return; }
    c.unlocked++;
    report(c, key, "unlocked", vi.format + " / " + vi.cipher);
    if (depth >= 4) { report(c, key + ".dec", "too-deep"); dec->close(); return; }
    walk(c, dec, key + ".dec", depth + 1);
    dec->close();
    return;
  }
  if (ps.code != Errc::Unsupported) {  // 暗号かどうか判定する読み取り自体が失敗 (切り詰め等)
    report(c, key, std::string("probe-error:") + errc_name(ps.code));
    return;
  }
  Status io;
  FsType t = probe_filesystem(*dev, 0, dev->size(), &io);
  if (t == FsType::Unknown) { report(c, key, io.ok() ? "unknown-fs" : std::string("probe-error:") + errc_name(io.code)); return; }
  int err = 0;
  auto vol = mount_fs(dev, t, &err);
  if (!vol) { report(c, key, "mount-failed", std::string(fs_type_name(t)) + " err " + std::to_string(err)); return; }
  auto ex = c.expects.find(key);
  if (ex == c.expects.end() || ex->second.manifest.empty()) { report(c, key, "mounted", fs_type_name(t)); return; }
  bool ok = false;
  auto man = load_manifest(ex->second.manifest, &ok);
  if (!ok || man.empty()) { fail(c, "node %s: cannot read manifest %s", key, ex->second.manifest); return; }
  Verify r = verify_manifest(*vol, man);
  char buf[160];
  std::snprintf(buf, sizeof buf, "%s: %ld files (%llu bytes), %ld dirs; errors %ld mismatches %ld extras %ld", vol->fs_name(), r.files, (unsigned long long)r.bytes, r.dirs, r.errors,
                r.mismatches, r.extras);
  if (r.mismatches || r.extras) report(c, key, "mismatch", buf);
  else if (r.errors) report(c, key, "partial", buf);
  else { c.verified++; report(c, key, "verified", buf); }
}

// デバイスにパーティション表があれば各パーティションを、無ければ全体を 1 つのボリュームとして調べる
void walk(Ctx& c, const BlockDevicePtr& dev, const std::string& key, int depth) {
  PartitionTable t;
  Status s = read_partition_table(*dev, &t);
  if (!s.ok()) { report(c, key, std::string("scan-error:") + errc_name(s.code)); return; }
  if (t.whole_device_volume) {
    report(c, key + "#table", "none");
    handle_volume(c, dev, key, depth);
    return;
  }
  if (t.kind == TableKind::None) {
    report(c, key + "#table", "none");
    handle_volume(c, dev, key, depth);  // 表も既知の FS も見えない: 暗号ヘッダ (先頭が壊れた等) かもしれないので同じ経路で判定する
    return;
  }
  report(c, key + "#table", t.kind == TableKind::Gpt ? (t.used_backup_gpt ? "gpt-backup" : "gpt") : "mbr", std::to_string(t.partitions.size()) + " entries");
  for (auto& d : t.diagnostics) if (d.severity == Severity::Error) std::printf("  scan diagnostic (error): %s\n", d.code.c_str());
  for (auto& p : t.partitions) {
    if (p.extended_container || p.protective) continue;
    const std::string k = key + ".p" + std::to_string(p.index);
    BlockDevicePtr sub;
    uint64_t len = p.size_bytes;
    auto cl = c.clip.find(k);
    if (cl != c.clip.end() && cl->second < len) len = cl->second;
    Status ss = SubRangeDevice::create(dev, p.start_bytes, len, true, &sub);
    if (!ss.ok()) { report(c, k, std::string("subrange-failed:") + errc_name(ss.code)); continue; }
    handle_volume(c, sub, k, depth);
  }
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 3 || std::strcmp(argv[1], "--scan") != 0) { std::fprintf(stderr, "usage: see header of test_cryptodisk.cpp\n"); return 2; }
  alarm(900);  // ハング検出 (ctest の TIMEOUT より短く、SIGALRM で異常終了させる)
  const std::string image = argv[2];
  Ctx c;
  uint64_t cut = 0;
  std::map<uint64_t, uint8_t> pokes;
  for (int i = 3; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--cred" && i + 1 < argc) {
      std::string spec = argv[++i], key;
      size_t eq = spec.find('=');
      if (eq != std::string::npos && eq < spec.find(':')) { key = spec.substr(0, eq); spec = spec.substr(eq + 1); }
      auto cr = std::make_shared<Credentials>();
      if (!parse_cred(spec, cr.get())) { std::fprintf(stderr, "bad credential spec\n"); return 2; }
      (key.empty() ? c.global_creds : c.bound[key]).push_back(cr);
    } else if (a == "--cut" && i + 1 < argc) {
      cut = std::strtoull(argv[++i], nullptr, 0);
    } else if (a == "--poke" && i + 1 < argc) {
      std::string spec = argv[++i];
      size_t eq = spec.find('=');
      if (eq == std::string::npos) return 2;
      uint64_t off = std::strtoull(spec.substr(0, eq).c_str(), nullptr, 0);
      std::string hex = spec.substr(eq + 1);
      for (size_t k = 0; k + 1 < hex.size(); k += 2) pokes[off + k / 2] = uint8_t(std::stoi(hex.substr(k, 2), nullptr, 16));
    } else if (a == "--clip" && i + 1 < argc) {
      std::string spec = argv[++i];
      size_t eq = spec.find('=');
      if (eq == std::string::npos) return 2;
      c.clip[spec.substr(0, eq)] = std::strtoull(spec.c_str() + eq + 1, nullptr, 0);
    } else if (a == "--lenient") {
      c.lenient = true;
    } else {
      size_t eq = a.find('=');
      if (eq == std::string::npos) { std::fprintf(stderr, "bad argument: %s\n", a.c_str()); return 2; }
      Expect e;
      std::string w = a.substr(eq + 1);
      size_t colon = w.find(':');
      if (w.compare(0, 9, "verified:") == 0) { e.what = "verified"; e.manifest = w.substr(9); }
      else { e.what = w; (void)colon; }
      c.expects[a.substr(0, eq)] = e;
    }
  }
  BlockDevicePtr raw;
  if (!RawImageDevice::open_path(image, true, 512, &raw).ok()) { std::fprintf(stderr, "cannot open %s\n", image.c_str()); return 2; }
  BlockDevicePtr dev = raw;
  if (cut || !pokes.empty()) dev = std::make_shared<MutDevice>(raw, pokes, cut);
  std::printf("disk %s: %llu bytes%s%s\n", image.c_str(), (unsigned long long)dev->size(), cut ? " (cut)" : "", pokes.empty() ? "" : " (poked)");
  walk(c, dev, "d", 0);
  for (auto& kv : c.expects) {
    if (kv.second.seen || kv.second.what == "safe") continue;
    fail(c, "node %s: expected %s but the node was never reached", kv.first, kv.second.what);
  }
  std::printf("cryptodisk: %d nodes, %d crypto volumes unlocked, %d file systems verified, %d failures\n", c.nodes, c.unlocked, c.verified, c.failures);
  return c.failures ? 1 : 0;
}
