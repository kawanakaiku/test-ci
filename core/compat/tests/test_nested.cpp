// SPDX-License-Identifier: GPL-2.0-or-later
// ネスト (ループバック) マウントの検証。docs/nested-mount.md
// フィクスチャは tools/gen_nested_fixture.py が作る (親: p_*.img の中に子: c_*.img / l*.img が入っている)。
//
// 使い方 (<parent> = 親イメージ、<prefix> = 親の中のイメージ置き場 "/imgs/" または "/"、<fix> = フィクスチャのディレクトリ。元の子ファイルもここにある):
//   --read       <parent> <prefix> <fix>   子ごとに: ネストのデバイスが元ファイルと全バイト一致 / ネストでマウントした木が直接マウントした木と一致
//   --partitions <parent> <prefix> <fix>   GPT / MBR(+論理) のイメージ: パーティション表と各パーティションの木が一致
//   --depth      <parent> <prefix> <fix>   l1 ⊃ l2 ⊃ l3 ⊃ l4: 3 段までは開け、4 段目は ELOOP。max_depth=4 なら 4 段も読める
//   --lifetime   <parent> <prefix> <fix>   親を先に手放しても子が読める / 子を閉じた後に親が解放される / close 後の I/O は失敗する
//   --cycle                                再入 (ループ) はデッドロックせず失敗する / 同じファイルを祖先が裏打ちしていれば ELOOP
//   --readonly   <parent> <prefix> <fix>   ネストのデバイスは書けない。子は書き込み不可 (mount_rw でも)
//   --misc       <parent> <prefix> <fix>   存在しない / ディレクトリ / 小さすぎるファイル / 中身が判別できないイメージの扱い
//   --ioerr      <parent> <prefix> <fix> <child>   親の読み出しエラー (持続 / 1 回だけ) を注入: 誤ったデータを返さず、クラッシュしない
//   --concurrent <parent> <prefix> <fix>   親と子を複数スレッドから同時に読む (デッドロック・不一致がない)
//   --stale      <parent> <prefix> <fix>   (書き込み有効ビルドのみ) 親が書き込み可能なとき、マウント中のイメージの書き換えを検出して Io(ESTALE)
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

#include "usbmem/devices.h"
#include "usbmem/exfat_volume.h"
#include "usbmem/ext2_volume.h"
#include "usbmem/ext4_volume.h"
#include "usbmem/fat_volume.h"
#include "usbmem/fs_probe.h"
#include "usbmem/fs_volume.h"
#include "usbmem/nested_mount.h"
#include "usbmem/ntfs_volume.h"
#include "usbmem/partition.h"

using namespace usbmem;

namespace {

int g_fail = 0;
#define CHECK(c, ...)                                                   \
  do {                                                                  \
    if (!(c)) {                                                         \
      g_fail++;                                                         \
      std::fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);        \
      std::fprintf(stderr, __VA_ARGS__);                                \
      std::fputc('\n', stderr);                                         \
    }                                                                   \
  } while (0)

BlockDevicePtr open_file(const std::string& p, bool ro = true) {
  BlockDevicePtr d;
  Status s = RawImageDevice::open_path(p, ro, 512, &d);
  if (!s.ok()) { std::fprintf(stderr, "cannot open %s\n", p.c_str()); std::exit(2); }
  return d;
}

// jni_ntfs.cpp の mount_auto と同じ振り分け (判別できなければ NTFS として試す)
std::unique_ptr<FsVolume> mount_raw(const BlockDevicePtr& dev, int* err, bool rw = false) {
  FsType t = probe_filesystem(*dev, 0, dev->size());
  if (t == FsType::Exfat) return rw ? ExfatVolume::mount_rw(dev, err) : ExfatVolume::mount(dev, err);
  if (t == FsType::Ext2) return rw ? Ext2Volume::mount_rw(dev, err) : Ext2Volume::mount(dev, err);
  if (t == FsType::Ext3 || t == FsType::Ext4) return Ext4Volume::mount(dev, err);
  if (t == FsType::Fat12 || t == FsType::Fat16 || t == FsType::Fat32) return FatVolume::mount(dev, err);
  return rw ? NtfsVolume::mount_rw(dev, err) : NtfsVolume::mount(dev, err);
}
std::shared_ptr<FsVolume> mount_locked(const BlockDevicePtr& dev, int* err = nullptr, bool rw = false) {
  int e = 0;
  auto v = mount_raw(dev, err ? err : &e, rw);
  return make_locked(std::move(v));
}

// ---- デバイスとファイルの比較
bool pread_all(int fd, uint64_t off, void* buf, size_t len) {
  auto* p = static_cast<uint8_t*>(buf);
  while (len) {
    ssize_t r = pread(fd, p, len, static_cast<off_t>(off));
    if (r <= 0) return false;
    p += r; off += static_cast<uint64_t>(r); len -= static_cast<size_t>(r);
  }
  return true;
}

bool dev_equals_file(BlockDevice& d, const std::string& file) {
  int fd = open(file.c_str(), O_RDONLY);
  if (fd < 0) return false;
  off_t fsz = lseek(fd, 0, SEEK_END);
  bool ok = static_cast<uint64_t>(fsz) == d.size();
  std::vector<uint8_t> a, b;
  auto cmp = [&](uint64_t off, size_t len) {
    a.resize(len); b.resize(len);
    if (!pread_all(fd, off, b.data(), len)) return false;
    Status s = d.read_at(off, a.data(), len);
    return s.ok() && std::memcmp(a.data(), b.data(), len) == 0;
  };
  // (1) 64 KiB ずつ全域、(2) 先頭 512 KiB を 4093 バイトずつ (ブロック境界をまたぐ)、(3) ランダムな範囲
  for (uint64_t off = 0; ok && off < d.size(); off += 65536) ok = cmp(off, static_cast<size_t>(std::min<uint64_t>(65536, d.size() - off)));
  for (uint64_t off = 0; ok && off < std::min<uint64_t>(d.size(), 512 * 1024); off += 4093) ok = cmp(off, static_cast<size_t>(std::min<uint64_t>(4093, d.size() - off)));
  std::mt19937_64 rng(12345);
  for (int i = 0; ok && i < 200; i++) {
    uint64_t off = rng() % d.size();
    size_t len = static_cast<size_t>(std::min<uint64_t>(d.size() - off, 1 + rng() % 100000));
    ok = cmp(off, len);
  }
  close(fd);
  return ok;
}

// ---- ツリーの比較 (a = 検査対象、b = 基準)。tolerant のとき a の読み取りエラーは errors に数える (誤ったデータだけが不一致)
struct Walk { long files = 0, dirs = 0, mism = 0, errors = 0; };

void walk(FsVolume& a, uint64_t ia, FsVolume& b, uint64_t ib, const std::string& path, Walk& w, bool tolerant, int depth = 0) {
  if (depth > 32) return;
  std::vector<FsDirEntry> ea, eb;
  int ra = a.readdir(ia, &ea);
  int rb = b.readdir(ib, &eb);
  if (rb) { std::fprintf(stderr, "reference readdir failed %s: %d\n", path.c_str(), rb); w.mism++; return; }
  if (ra) { if (tolerant) w.errors++; else { w.mism++; std::fprintf(stderr, "readdir %s failed: %d\n", path.c_str(), ra); } return; }
  auto key = [](std::vector<FsDirEntry>& v) {
    std::map<std::string, FsDirEntry> m;
    for (auto& e : v) if (e.name != "." && e.name != "..") m[e.name] = e;
    return m;
  };
  auto ma = key(ea), mb = key(eb);
  for (auto& [name, eb1] : mb) {
    auto it = ma.find(name);
    if (it == ma.end()) { w.mism++; std::fprintf(stderr, "missing in nested: %s/%s\n", path.c_str(), name.c_str()); continue; }
    FsStat sa, sb;
    int r1 = a.stat(it->second.ino, &sa), r2 = b.stat(eb1.ino, &sb);
    if (r2) { w.mism++; continue; }
    if (r1) { if (tolerant) w.errors++; else { w.mism++; std::fprintf(stderr, "stat failed %s/%s\n", path.c_str(), name.c_str()); } continue; }
    if (sa.mode != sb.mode || sa.size != sb.size || sa.mtime != sb.mtime) {
      w.mism++; std::fprintf(stderr, "attr mismatch %s/%s: mode %o/%o size %llu/%llu mtime %lld/%lld\n", path.c_str(), name.c_str(), sa.mode, sb.mode,
                             (unsigned long long)sa.size, (unsigned long long)sb.size, (long long)sa.mtime, (long long)sb.mtime);
      continue;
    }
    if (sb.is_dir()) { w.dirs++; walk(a, it->second.ino, b, eb1.ino, path + "/" + name, w, tolerant, depth + 1); continue; }
    if ((sb.mode & 0170000) != 0100000) continue;  // 通常ファイルだけ中身を比べる
    w.files++;
    std::vector<uint8_t> x(65536), y(65536);
    for (uint64_t off = 0; off < sb.size; off += 65536) {
      size_t n = static_cast<size_t>(std::min<uint64_t>(65536, sb.size - off));
      int64_t rr = b.read(eb1.ino, off, y.data(), n);
      int64_t qq = a.read(it->second.ino, off, x.data(), n);
      if (rr != static_cast<int64_t>(n)) { w.mism++; break; }
      if (tolerant && qq < static_cast<int64_t>(n)) {
        // エラー (負) か、エラーの手前までの短い読み。短く読めた分は正しいデータでなければならない
        w.errors++;
        if (qq > 0 && std::memcmp(x.data(), y.data(), static_cast<size_t>(qq)) != 0) { w.mism++; std::fprintf(stderr, "short read data mismatch %s/%s @%llu\n", path.c_str(), name.c_str(), (unsigned long long)off); }
        break;
      }
      if (qq != static_cast<int64_t>(n) || std::memcmp(x.data(), y.data(), n) != 0) {
        w.mism++; std::fprintf(stderr, "data mismatch %s/%s @%llu (read %lld)\n", path.c_str(), name.c_str(), (unsigned long long)off, (long long)qq); break;
      }
    }
  }
  if (!tolerant && ma.size() != mb.size()) { w.mism++; std::fprintf(stderr, "extra entries in nested dir %s\n", path.c_str()); }
}

Walk compare_trees(FsVolume& a, FsVolume& b, bool tolerant) {
  Walk w;
  walk(a, a.root(), b, b.root(), "", w, tolerant);
  return w;
}

struct Ctx {
  std::string parent, prefix, fix;
  std::string child_path(const std::string& n) const { return prefix + n; }
  std::string orig(const std::string& n) const { return fix + "/" + n; }
};

// 子 (名前 n) をネストで開く。親は呼び出し側が保持する。
struct Nested { std::shared_ptr<FsVolume> parent; NestedOpen open; };

// ---------------------------------------------------------------- --read
int cmd_read(const Ctx& c) {
  auto pv = mount_locked(open_file(c.parent));
  CHECK(pv != nullptr, "mount parent %s", c.parent.c_str());
  if (!pv) return 1;
  // 全域一致する子
  const char* whole[] = {"c_ext2.img", "c_ext4.img", "c_fat.img", "c_ntfs.img"};
  for (const char* n : whole) {
    NestedOpen o = open_nested_image(pv, nullptr, c.child_path(n));
    CHECK(o.err == 0, "%s: open_nested_image err=%d (%s)", n, o.err, o.why.c_str());
    if (o.err) continue;
    CHECK(o.dev->is_read_only(), "%s: must be read-only", n);
    CHECK(o.lineage && o.lineage->depth == 1, "%s: depth", n);
    CHECK(dev_equals_file(*o.dev, c.orig(n)), "%s: nested device differs from the original file", n);
    int e1 = 0, e2 = 0;
    auto cv = mount_locked(o.dev, &e1);
    auto rv = mount_locked(open_file(c.orig(n)), &e2);
    CHECK(rv != nullptr, "%s: reference mount (err %d)", n, e2);
    CHECK(cv != nullptr, "%s: nested mount failed (err %d)", n, e1);
    if (cv && rv) {
      CHECK(std::strcmp(cv->fs_name(), rv->fs_name()) == 0, "%s: fs name %s vs %s", n, cv->fs_name(), rv->fs_name());
      CHECK(!cv->writable(), "%s: child must be read-only", n);
      Walk w = compare_trees(*cv, *rv, false);
      CHECK(w.mism == 0 && w.errors == 0 && w.files > 20, "%s: tree compare mism=%ld errors=%ld files=%ld", n, w.mism, w.errors, w.files);
      std::printf("  %-12s fs=%s files=%ld dirs=%ld ok\n", n, cv->fs_name(), w.files, w.dirs);
    }
  }
  // パーティション表付き・中身が判別できないイメージも、デバイスとしては元と一致する
  for (const char* n : {"c_gpt.img", "c_mbr.img", "c_empty.img"}) {
    NestedOpen o = open_nested_image(pv, nullptr, c.child_path(n));
    CHECK(o.err == 0, "%s: open err=%d (%s)", n, o.err, o.why.c_str());
    if (!o.err) CHECK(dev_equals_file(*o.dev, c.orig(n)), "%s: nested device differs from the original file", n);
  }
  return 0;
}

// ---------------------------------------------------------------- --partitions
bool same_part(const Partition& a, const Partition& b) {
  return a.index == b.index && a.start_bytes == b.start_bytes && a.size_bytes == b.size_bytes && a.logical == b.logical && a.extended_container == b.extended_container &&
         a.mbr_type == b.mbr_type && a.type_guid == b.type_guid && a.unique_guid == b.unique_guid && a.name == b.name && a.fs == b.fs && a.fs_label == b.fs_label &&
         a.fs_uuid == b.fs_uuid && a.part_uuid == b.part_uuid;
}

int cmd_partitions(const Ctx& c) {
  auto pv = mount_locked(open_file(c.parent));
  CHECK(pv != nullptr, "mount parent");
  if (!pv) return 1;
  for (const char* n : {"c_gpt.img", "c_mbr.img"}) {
    NestedOpen o = open_nested_image(pv, nullptr, c.child_path(n));
    CHECK(o.err == 0, "%s: open err=%d", n, o.err);
    if (o.err) continue;
    auto ref = open_file(c.orig(n));
    PartitionTable tn, tr;
    CHECK(read_partition_table(*o.dev, &tn).ok(), "%s: nested partition scan", n);
    CHECK(read_partition_table(*ref, &tr).ok(), "%s: reference partition scan", n);
    bool gpt = std::string(n) == "c_gpt.img";
    CHECK(tn.kind == (gpt ? TableKind::Gpt : TableKind::Mbr), "%s: table kind", n);
    CHECK(tn.kind == tr.kind && tn.disk_bytes == tr.disk_bytes && tn.partitions.size() == tr.partitions.size(), "%s: table header/size", n);
    CHECK(tn.partitions.size() == (gpt ? 2u : 4u), "%s: partition count %zu", n, tn.partitions.size());
    for (size_t i = 0; i < std::min(tn.partitions.size(), tr.partitions.size()); i++) CHECK(same_part(tn.partitions[i], tr.partitions[i]), "%s: partition #%zu differs", n, i);
    int mounted = 0;
    for (auto& p : tn.partitions) {
      if (p.extended_container) continue;
      BlockDevicePtr sn, sr;
      CHECK(SubRangeDevice::create(o.dev, p.start_bytes, p.size_bytes, true, &sn).ok(), "%s #%u: sub-range", n, p.index);
      CHECK(SubRangeDevice::create(ref, p.start_bytes, p.size_bytes, true, &sr).ok(), "%s #%u: ref sub-range", n, p.index);
      if (!sn || !sr) continue;
      int e1 = 0, e2 = 0;
      auto cv = mount_locked(sn, &e1);
      auto rv = mount_locked(sr, &e2);
      CHECK(cv && rv, "%s #%u: mount nested=%d ref=%d", n, p.index, e1, e2);
      if (!cv || !rv) continue;
      CHECK(!cv->writable(), "%s #%u: must be read-only", n, p.index);
      Walk w = compare_trees(*cv, *rv, false);
      CHECK(w.mism == 0 && w.errors == 0 && w.files > 20, "%s #%u: tree compare mism=%ld errors=%ld files=%ld", n, p.index, w.mism, w.errors, w.files);
      mounted++;
    }
    CHECK(mounted == (gpt ? 2 : 3), "%s: mounted %d partitions", n, mounted);
    std::printf("  %-12s %s: %zu entries, %d mounted ok\n", n, gpt ? "GPT" : "MBR", tn.partitions.size(), mounted);
  }
  return 0;
}

// ---------------------------------------------------------------- --depth
int cmd_depth(const Ctx& c) {
  if (c.prefix != "/imgs/") { std::printf("  (skip: chain only in the ext4 parent)\n"); return 0; }
  auto pv = mount_locked(open_file(c.parent));
  CHECK(pv != nullptr, "mount parent");
  if (!pv) return 1;
  const char* names[] = {"l1.img", "l2.img", "l3.img", "l4.img"};
  std::vector<std::shared_ptr<FsVolume>> vols{pv};
  std::vector<NestedOpen> opens;
  std::shared_ptr<const NestedLineage> lin;
  for (int k = 0; k < 3; k++) {
    std::string path = k == 0 ? c.child_path("l1.img") : std::string("/") + names[k];
    NestedOpen o = open_nested_image(vols.back(), lin, path);
    CHECK(o.err == 0, "level %d (%s): err=%d (%s)", k + 1, path.c_str(), o.err, o.why.c_str());
    if (o.err) return 1;
    CHECK(o.lineage->depth == k + 1, "level %d: depth %d", k + 1, o.lineage->depth);
    CHECK(dev_equals_file(*o.dev, c.orig(names[k])), "level %d: device differs from %s", k + 1, names[k]);
    int e = 0;
    auto v = mount_locked(o.dev, &e);
    CHECK(v != nullptr, "level %d: mount err %d", k + 1, e);
    if (!v) return 1;
    CHECK(!v->writable(), "level %d: read-only", k + 1);
    auto rv = mount_locked(open_file(c.orig(names[k])));
    Walk w = compare_trees(*v, *rv, false);
    CHECK(w.mism == 0 && w.errors == 0, "level %d: tree compare mism=%ld", k + 1, w.mism);
    lin = o.lineage;
    opens.push_back(o);
    vols.push_back(v);
  }
  // 4 段目は既定の上限 (3) で拒否される
  NestedOpen o4 = open_nested_image(vols.back(), lin, "/l4.img");
  CHECK(o4.err == ELOOP && o4.why.find("deep") != std::string::npos, "level 4 must be refused with ELOOP (err=%d, %s)", o4.err, o4.why.c_str());
  CHECK(!o4.dev, "level 4: no device on failure");
  // 上限を 4 にすれば開け、読める (段数の制限は設定であって、実装の限界ではない)
  NestedOpen o4b = open_nested_image(vols.back(), lin, "/l4.img", 4);
  CHECK(o4b.err == 0, "level 4 with max_depth=4: err=%d", o4b.err);
  if (!o4b.err) {
    CHECK(o4b.lineage->depth == 4, "depth 4");
    CHECK(dev_equals_file(*o4b.dev, c.orig("l4.img")), "level 4: device differs");
    auto v4 = mount_locked(o4b.dev);
    auto r4 = mount_locked(open_file(c.orig("c_ext2.img")));
    CHECK(v4 && r4, "level 4: mount");
    if (v4 && r4) { Walk w = compare_trees(*v4, *r4, false); CHECK(w.mism == 0 && w.files > 20, "level 4: tree compare"); }
  }
  // max_depth=0 はどこからも開けない
  CHECK(open_nested_image(pv, nullptr, c.child_path("c_ext2.img"), 0).err == ELOOP, "max_depth=0");
  std::printf("  depth 1..3 ok, 4 refused (ELOOP), max_depth=4 ok\n");
  return 0;
}

// ---------------------------------------------------------------- --lifetime
int cmd_lifetime(const Ctx& c) {
  auto raw = open_file(c.parent);
  auto pv = mount_locked(raw);
  CHECK(pv != nullptr, "mount parent");
  if (!pv) return 1;
  std::weak_ptr<FsVolume> wparent = pv;
  NestedOpen o = open_nested_image(pv, nullptr, c.child_path("c_ext2.img"));
  CHECK(o.err == 0, "open");
  if (o.err) return 1;
  auto cv = mount_locked(o.dev);
  auto rv = mount_locked(open_file(c.orig("c_ext2.img")));
  CHECK(cv && rv, "mount");
  if (!cv || !rv) return 1;
  // Kotlin が親を先に閉じた場合に相当: 親の持ち主 (呼び出し側) を全部手放しても、子が親を保持するのでメモリは壊れず、読める
  pv.reset();
  raw.reset();
  CHECK(!wparent.expired(), "the child must keep the parent volume alive");
  Walk w = compare_trees(*cv, *rv, false);
  CHECK(w.mism == 0 && w.errors == 0 && w.files > 20, "child readable after the parent owner is gone (mism=%ld)", w.mism);
  // デバイスを close すると、以降の I/O は失敗する (新しいマウントは読めずに失敗するだけ。クラッシュしない)
  o.dev->close();
  uint8_t b[512];
  CHECK(o.dev->read_at(0, b, 512).code == Errc::Closed, "read after close must report Closed");
  int e = 0;
  auto again = mount_locked(o.dev, &e);
  CHECK(again == nullptr && e != 0, "mount on a closed device must fail (err=%d)", e);
  // 子を全部手放すと親も解放される
  cv.reset();
  o.dev.reset();
  o.lineage.reset();
  CHECK(wparent.expired(), "the parent must be released once the child is gone");
  std::printf("  lifetime ok\n");
  return 0;
}

// ---------------------------------------------------------------- --cycle
// 巡り巡って同じボリュームを読みに戻る FS (= ネストの循環)
class LoopFs final : public FsVolume {
 public:
  BlockDevicePtr dev;
  const char* fs_name() const override { return "loop"; }
  uint64_t root() const override { return 1; }
  int lookup(uint64_t, const std::string& n, uint64_t* ino) override { if (n != "x") return -2; *ino = 2; return 0; }
  int stat(uint64_t ino, FsStat* st) override { *st = FsStat(); st->ino = ino; st->mode = ino == 1 ? 0040755 : 0100644; st->size = 1 << 20; st->nlink = 1; return 0; }
  int readdir(uint64_t, std::vector<FsDirEntry>*) override { return 0; }
  int64_t read(uint64_t, uint64_t off, void* buf, size_t len) override {
    Status s = dev->read_at(off, buf, len);  // 自分自身を裏打ちにしたデバイスを読む → 再入
    return s.ok() ? static_cast<int64_t>(len) : -EIO;
  }
};

int cmd_cycle() {
  // (1) 実行時の再入: デッドロックせず失敗する
  auto lf = std::make_unique<LoopFs>();
  LoopFs* raw = lf.get();
  auto lv = make_locked(std::move(lf));
  NestedOpen o = open_nested_image(lv, nullptr, "/x");
  CHECK(o.err == 0, "open loop file: %d (%s)", o.err, o.why.c_str());
  if (!o.err) {
    raw->dev = o.dev;
    uint8_t b[512];
    alarm(20);  // デッドロックしたらここで落ちる
    Status s = o.dev->read_at(0, b, 512);
    alarm(0);
    CHECK(s.code == Errc::Io, "re-entrant read must fail with Io (got %s)", errc_name(s.code));
    // 別スレッドからの通常の読みは再入ではない (同じ失敗になるが、ロックは取れる)
    Status s2;
    std::thread t([&] { s2 = o.dev->read_at(0, b, 512); });
    t.join();
    CHECK(s2.code == Errc::Io, "second thread: %s", errc_name(s2.code));
    raw->dev.reset();  // 循環した参照を切る
  }
  // (2) 系譜: 祖先がすでにそのファイルを裏打ちにしていれば拒否 (ELOOP)
  auto base = std::make_unique<LoopFs>();
  auto bv = make_locked(std::move(base));
  auto lin = std::make_shared<NestedLineage>();
  lin->depth = 1;
  lin->chain.push_back({bv.get(), 2});
  NestedOpen c = open_nested_image(bv, lin, "/x");
  CHECK(c.err == ELOOP && c.why.find("cycle") != std::string::npos, "cycle must be refused (err=%d %s)", c.err, c.why.c_str());
  lin->chain.clear();
  lin->chain.push_back({bv.get(), 3});  // 別のファイルなら通る
  NestedOpen ok = open_nested_image(bv, lin, "/x");
  CHECK(ok.err == 0, "a different file is not a cycle (err=%d)", ok.err);
  std::printf("  cycle ok\n");
  return 0;
}

// ---------------------------------------------------------------- --readonly
int cmd_readonly(const Ctx& c) {
  auto pv = mount_locked(open_file(c.parent));
  CHECK(pv != nullptr, "mount parent");
  if (!pv) return 1;
  NestedOpen o = open_nested_image(pv, nullptr, c.child_path("c_ext2.img"));
  CHECK(o.err == 0, "open");
  if (o.err) return 1;
  uint8_t b[512] = {1};
  CHECK(o.dev->write_at(0, b, 512).code == Errc::ReadOnly, "write_at must be refused");
  CHECK(o.dev->is_read_only(), "is_read_only");
  // 書き込み可を要求してマウントしても、書き込み可にはならない (mount_rw は失敗するか、読み取り専用に落ちる)
  for (const char* n : {"c_ext2.img", "c_ntfs.img"}) {
    NestedOpen on = open_nested_image(pv, nullptr, c.child_path(n));
    if (on.err) { CHECK(false, "open %s", n); continue; }
    int e = 0;
    auto v = mount_locked(on.dev, &e, true);
    CHECK(v == nullptr || !v->writable(), "%s: a child of a nested image must never be writable", n);
    if (v) {
      uint64_t ino;
      CHECK(v->create(v->root(), "newfile", 0644, &ino) != 0, "%s: create must fail", n);
      CHECK(v->unlink(v->root(), "hello.txt") != 0, "%s: unlink must fail", n);
    }
  }
  std::printf("  readonly ok\n");
  return 0;
}

// ---------------------------------------------------------------- --misc
int cmd_misc(const Ctx& c) {
  auto pv = mount_locked(open_file(c.parent));
  CHECK(pv != nullptr, "mount parent");
  if (!pv) return 1;
  CHECK(open_nested_image(pv, nullptr, c.child_path("no_such.img")).err == ENOENT, "missing file -> ENOENT");
  CHECK(open_nested_image(pv, nullptr, c.prefix).err == EISDIR, "directory -> EISDIR");
  if (c.prefix == "/imgs/") {
    NestedOpen t = open_nested_image(pv, nullptr, "/docs/hello.txt");
    CHECK(t.err == EINVAL, "tiny file -> EINVAL (got %d)", t.err);
  }
  CHECK(open_nested_image(nullptr, nullptr, "/x").err == EINVAL, "null parent");
  // 中身が判別できないイメージ: マウントは失敗するだけ (クラッシュ・ハングしない)
  NestedOpen e = open_nested_image(pv, nullptr, c.child_path("c_empty.img"));
  CHECK(e.err == 0, "open c_empty");
  if (!e.err) {
    int err = 0;
    auto v = mount_locked(e.dev, &err);
    CHECK(v == nullptr && err != 0, "an all-zero image must not mount (err=%d)", err);
    PartitionTable t;
    CHECK(read_partition_table(*e.dev, &t).ok(), "partition scan on an all-zero image");
    CHECK(t.partitions.empty() && !t.whole_device_volume, "no partitions in an all-zero image");
  }
  // 範囲外 / 境界
  NestedOpen o = open_nested_image(pv, nullptr, c.child_path("c_ext2.img"));
  CHECK(o.err == 0, "open");
  if (!o.err) {
    uint8_t b[1024];
    CHECK(o.dev->read_at(o.dev->size() - 512, b, 512).ok(), "last sector");
    CHECK(o.dev->read_at(o.dev->size() - 511, b, 512).code == Errc::OutOfRange, "beyond end");
    CHECK(o.dev->read_at(~0ull - 100, b, 512).code == Errc::OutOfRange, "offset overflow");
    CHECK(o.dev->read_at(o.dev->size(), b, 0).ok(), "empty read at end");
    CancelToken tok; tok.cancel(); IoOptions opt; opt.cancel = &tok;
    CHECK(o.dev->read_at(0, b, 512, opt).code == Errc::Cancelled, "cancel");
    CHECK(o.dev->describe().find("nested:") == 0, "describe");
  }
  std::printf("  misc ok\n");
  return 0;
}

// ---------------------------------------------------------------- --ioerr
// 読み出しを数え、N 回目以降 (sticky) または N 回目だけ (once) 失敗させる
class FaultDevice final : public BlockDevice {
 public:
  FaultDevice(BlockDevicePtr p, long fail_at, bool sticky) : p_(std::move(p)), at_(fail_at), sticky_(sticky) {}
  Status read_at(uint64_t off, void* buf, size_t len, const IoOptions& o = {}) override {
    long n = reads_.fetch_add(1);
    if (at_ >= 0 && (n == at_ || (sticky_ && n > at_))) return Status::Make(Errc::Io, EIO);
    return p_->read_at(off, buf, len, o);
  }
  Status write_at(uint64_t, const void*, size_t, const IoOptions& = {}) override { return Status::Make(Errc::ReadOnly); }
  Status flush() override { return Status::Ok(); }
  Status close() override { return Status::Ok(); }
  uint64_t size() const override { return p_->size(); }
  uint32_t logical_block_size() const override { return p_->logical_block_size(); }
  bool is_read_only() const override { return true; }
  std::string describe() const override { return "fault"; }
  long reads() const { return reads_.load(); }
 private:
  BlockDevicePtr p_; long at_; bool sticky_; std::atomic<long> reads_{0};
};

// 1 回の試行: 親の下位デバイスの N 回目の読みを失敗させ、親のマウント → ネスト → 子のマウント → 木の比較を一通り行う
// 戻り値: 0 = 失敗に気づいた (エラーが返った)、1 = 何も起きなかった。不一致 (誤ったデータ) は g_fail に数える。
int ioerr_trial(const Ctx& c, const std::string& child, long fail_at, bool sticky, long* total_reads) {
  auto fd = std::make_shared<FaultDevice>(open_file(c.parent), fail_at, sticky);
  int err = 0;
  auto pv = mount_locked(fd, &err);
  int noticed = 0;
  if (!pv) { noticed = 1; }
  else {
    NestedOpen o = open_nested_image(pv, nullptr, c.child_path(child));
    if (o.err) { noticed = 1; }
    else {
      int e2 = 0;
      auto cv = mount_locked(o.dev, &e2);
      if (!cv) { noticed = 1; }
      else {
        auto rv = mount_locked(open_file(c.orig(child)));
        Walk w = compare_trees(*cv, *rv, true);
        CHECK(w.mism == 0, "fail_at=%ld sticky=%d: %ld mismatched results (wrong data must never be returned)", fail_at, sticky, w.mism);
        if (w.errors) noticed = 1;
        // デバイス全体も、読めた範囲は元と一致する
        int ofd = open(c.orig(child).c_str(), O_RDONLY);
        std::vector<uint8_t> a(65536), b(65536);
        for (uint64_t off = 0; ofd >= 0 && off < o.dev->size(); off += 65536) {
          size_t n = static_cast<size_t>(std::min<uint64_t>(65536, o.dev->size() - off));
          Status s = o.dev->read_at(off, a.data(), n);
          if (!s.ok()) { noticed = 1; CHECK(s.code == Errc::Io, "error code %s", errc_name(s.code)); continue; }
          if (!pread_all(ofd, off, b.data(), n) || std::memcmp(a.data(), b.data(), n) != 0) CHECK(false, "fail_at=%ld: device data mismatch @%llu", fail_at, (unsigned long long)off);
        }
        if (ofd >= 0) close(ofd);
      }
    }
  }
  if (total_reads) *total_reads = fd->reads();
  return noticed ? 0 : 1;
}

int cmd_ioerr(const Ctx& c, const std::string& child) {
  long total = 0;
  CHECK(ioerr_trial(c, child, -1, true, &total) == 1, "baseline must be clean");
  CHECK(total > 10, "baseline read count %ld", total);
  int noticed_sticky = 0, noticed_once = 0, trials = 0;
  std::vector<long> points;
  for (long n = 0; n < 40 && n < total; n++) points.push_back(n);
  for (long n = 40; n < total; n += std::max<long>(1, total / 60)) points.push_back(n);
  for (long n : points) {
    trials++;
    if (ioerr_trial(c, child, n, true, nullptr) == 0) noticed_sticky++;
    if (ioerr_trial(c, child, n, false, nullptr) == 0) noticed_once++;
  }
  CHECK(noticed_sticky >= trials / 2, "sticky errors were noticed in only %d of %d trials", noticed_sticky, trials);
  std::printf("  ioerr %s: %ld reads in the baseline, %d trials, errors noticed sticky=%d once=%d, no wrong data\n", child.c_str(), total, trials, noticed_sticky, noticed_once);
  return 0;
}

// ---------------------------------------------------------------- --concurrent
void on_alarm(int) { const char m[] = "TIMEOUT: probable deadlock\n"; (void)!write(2, m, sizeof m - 1); _exit(3); }

int cmd_concurrent(const Ctx& c) {
  signal(SIGALRM, on_alarm);
  alarm(240);
  auto pv = mount_locked(open_file(c.parent));
  CHECK(pv != nullptr, "mount parent");
  if (!pv) return 1;
  NestedOpen o = open_nested_image(pv, nullptr, c.child_path("c_ext2.img"));
  CHECK(o.err == 0, "open");
  if (o.err) return 1;
  auto cv = mount_locked(o.dev);
  auto rv = mount_locked(open_file(c.orig("c_ext2.img")));
  CHECK(cv && rv, "mount");
  if (!cv || !rv) return 1;
  std::atomic<long> bad{0};
  std::vector<std::thread> th;
  // 子のツリーを 2 スレッドで何度も比較
  for (int i = 0; i < 2; i++) th.emplace_back([&] { for (int k = 0; k < 6; k++) { Walk w = compare_trees(*cv, *rv, false); if (w.mism || w.errors) bad++; } });
  // 親のイメージファイルを直接 (SAF プロバイダ相当の経路で) 読む 2 スレッド
  uint64_t pino = 0;
  CHECK(pv->resolve(c.child_path("c_ext2.img"), &pino) == 0, "resolve");
  int ofd = open(c.orig("c_ext2.img").c_str(), O_RDONLY);
  for (int i = 0; i < 2; i++) th.emplace_back([&, i] {
    std::mt19937_64 rng(100 + i);
    std::vector<uint8_t> a(70000), b(70000);
    for (int k = 0; k < 400; k++) {
      uint64_t off = rng() % (3 * 1024 * 1024 - 1);
      size_t n = static_cast<size_t>(std::min<uint64_t>(3 * 1024 * 1024 - off, 1 + rng() % 70000));
      int64_t r = pv->read(pino, off, a.data(), n);
      if (r != static_cast<int64_t>(n) || !pread_all(ofd, off, b.data(), n) || std::memcmp(a.data(), b.data(), n) != 0) bad++;
    }
  });
  // 新しいネストデバイスを開いて読む 1 スレッド
  th.emplace_back([&] {
    std::mt19937_64 rng(7);
    std::vector<uint8_t> a(50000), b(50000);
    for (int k = 0; k < 100; k++) {
      NestedOpen x = open_nested_image(pv, nullptr, c.child_path("c_ext2.img"));
      if (x.err) { bad++; continue; }
      uint64_t off = rng() % (3 * 1024 * 1024 - 1);
      size_t n = static_cast<size_t>(std::min<uint64_t>(3 * 1024 * 1024 - off, 1 + rng() % 50000));
      if (!x.dev->read_at(off, a.data(), n).ok() || !pread_all(ofd, off, b.data(), n) || std::memcmp(a.data(), b.data(), n) != 0) bad++;
    }
  });
  for (auto& t : th) t.join();
  close(ofd);
  alarm(0);
  CHECK(bad == 0, "%ld concurrent mismatches/errors", bad.load());
  std::printf("  concurrent ok\n");
  return 0;
}

// ---------------------------------------------------------------- --stale (書き込み有効ビルド)
int cmd_stale(const Ctx& c) {
#ifdef USBMEM_NESTED_WRITE
  if (c.prefix != "/imgs/" || c.parent.find("p_ext2") == std::string::npos) { std::printf("  (skip: ext2 parent only)\n"); return 0; }
  std::string tmp = c.fix + "/stale_tmp.img";
  {
    std::string cmd = "cp --reflink=auto '" + c.parent + "' '" + tmp + "'";
    if (std::system(cmd.c_str()) != 0) { std::fprintf(stderr, "cp failed\n"); return 2; }
  }
  int err = 0;
  auto pv = make_locked(Ext2Volume::mount_rw(open_file(tmp, false), &err));
  CHECK(pv && pv->writable(), "mount parent rw (err %d)", err);
  if (!pv || !pv->writable()) { unlink(tmp.c_str()); return 1; }
  NestedOpen o = open_nested_image(pv, nullptr, c.child_path("c_ext2.img"));
  CHECK(o.err == 0, "open");
  if (o.err) { unlink(tmp.c_str()); return 1; }
  uint8_t b[4096];
  CHECK(o.dev->read_at(0, b, 4096).ok(), "read before the modification");
  uint64_t ino = 0;
  CHECK(pv->resolve(c.child_path("c_ext2.img"), &ino) == 0, "resolve");
  uint8_t x = 0x5a;
  CHECK(pv->write(ino, 100, &x, 1) == 1, "modify the backing file through the parent");
  pv->sync();
  Status s = o.dev->read_at(0, b, 4096);
  CHECK(s.code == Errc::Io && s.sys_errno == ESTALE, "a modified backing file must be detected (code %s errno %d)", errc_name(s.code), s.sys_errno);
  // 新しく開き直せば、新しい内容が読める (古い子とは別物)
  NestedOpen o2 = open_nested_image(pv, nullptr, c.child_path("c_ext2.img"));
  CHECK(o2.err == 0 && o2.dev->read_at(0, b, 4096).ok() && b[100] == 0x5a, "a fresh open sees the new content");
  unlink(tmp.c_str());
  std::printf("  stale detection ok\n");
#else
  (void)c;
  std::printf("  (skip: built without write support)\n");
#endif
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc < 2) { std::fprintf(stderr, "usage: see the top of test_nested.cpp\n"); return 2; }
  std::string cmd = argv[1];
  if (cmd == "--cycle") { cmd_cycle(); return g_fail ? 1 : 0; }
  if (argc < 5) { std::fprintf(stderr, "usage: %s %s <parent> <prefix> <fix>\n", argv[0], cmd.c_str()); return 2; }
  Ctx c{argv[2], argv[3], argv[4]};
  std::printf("%s %s (prefix %s)\n", cmd.c_str(), c.parent.c_str(), c.prefix.c_str());
  if (cmd == "--read") cmd_read(c);
  else if (cmd == "--partitions") cmd_partitions(c);
  else if (cmd == "--depth") cmd_depth(c);
  else if (cmd == "--lifetime") cmd_lifetime(c);
  else if (cmd == "--readonly") cmd_readonly(c);
  else if (cmd == "--misc") cmd_misc(c);
  else if (cmd == "--ioerr") cmd_ioerr(c, argc > 5 ? argv[5] : "c_ext2.img");
  else if (cmd == "--concurrent") cmd_concurrent(c);
  else if (cmd == "--stale") cmd_stale(c);
  else { std::fprintf(stderr, "unknown command %s\n", cmd.c_str()); return 2; }
  if (g_fail) std::fprintf(stderr, "%d check(s) failed\n", g_fail);
  return g_fail ? 1 : 0;
}
