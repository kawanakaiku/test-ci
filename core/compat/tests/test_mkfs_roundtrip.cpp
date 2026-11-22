// SPDX-License-Identifier: GPL-2.0-or-later
// 自前の mkfs で作った FS をアプリのドライバ (カーネル由来。個人版だけ) で読み書きして戻す往復テスト (docs/mkfs-extended.md)。
//   usbmem_mkfs_roundtrip <ext2|ext3|ext4|fat12|fat16|fat32|exfat|ntfs> <image> [--cluster N] [--label L] [--sector N] [--features F] [--ro]
// イメージは呼び出し側が用意した空のファイル (tmpfs)。流れ: mkfs (計画 → 適用 → 検査 clean) → 読み書きでマウント → ディレクトリ・ファイル作成・追記・改名・削除 → unmount
// → 読み取り専用で再マウントして内容を突き合わせ → 自前の検査が clean。外部の fsck (e2fsck / fsck.fat / fsck.exfat / ntfsfix) は呼び出し側 (tools/run_mkfs_roundtrip.sh) が最後に走らせる。
// --ro: 書き込みビルドでなくても動くよう、マウントして空のルートを読むだけ。
// 終了コード: 0 成功 / 1 失敗 / 77 書き込みがビルドで無効 (SKIP)
#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "usbmem/devices.h"
#include "usbmem/exfat_volume.h"
#include "usbmem/ext2_volume.h"
#include "usbmem/ext4_volume.h"
#include "usbmem/fat_volume.h"
#include "usbmem/fscheck.h"
#include "usbmem/mkfs.h"
#include "usbmem/ntfs_volume.h"

using namespace usbmem;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_RC(e) do { int rc__ = (e); if (rc__ < 0) { std::fprintf(stderr, "CHECK_RC failed %s:%d: %s -> %d\n", __FILE__, __LINE__, #e, rc__); g_fail++; } } while (0)

static std::vector<uint8_t> pattern(size_t n, unsigned seed) {
  std::vector<uint8_t> b(n);
  for (size_t i = 0; i < n; i++) b[i] = static_cast<uint8_t>((i * 7 + seed) & 255);
  return b;
}

enum class Kind { Ext2, Ext4, Fat, Exfat, Ntfs };

static std::unique_ptr<FsVolume> mount(Kind k, BlockDevicePtr dev, bool rw, int* err) {
  *err = 0;
  switch (k) {
    case Kind::Ext2: return rw ? std::unique_ptr<FsVolume>(Ext2Volume::mount_rw(dev, err)) : std::unique_ptr<FsVolume>(Ext2Volume::mount(dev, err));
    case Kind::Ext4: return rw ? std::unique_ptr<FsVolume>(Ext4Volume::mount_rw(dev, err)) : std::unique_ptr<FsVolume>(Ext4Volume::mount(dev, err));
    case Kind::Fat: return rw ? std::unique_ptr<FsVolume>(FatVolume::mount_rw(dev, err)) : std::unique_ptr<FsVolume>(FatVolume::mount(dev, err));
    case Kind::Exfat: return rw ? std::unique_ptr<FsVolume>(ExfatVolume::mount_rw(dev, err)) : std::unique_ptr<FsVolume>(ExfatVolume::mount(dev, err));
    case Kind::Ntfs: return rw ? std::unique_ptr<FsVolume>(NtfsVolume::mount_rw(dev, err)) : std::unique_ptr<FsVolume>(NtfsVolume::mount(dev, err));
  }
  return nullptr;
}

// 期待する中身 (パス → 内容)
using Model = std::map<std::string, std::vector<uint8_t>>;

static void verify_tree(FsVolume& v, const Model& m, const std::vector<std::string>& dirs, const char* when) {
  for (auto& d : dirs) {
    uint64_t ino;
    int rc = v.resolve(d, &ino);
    if (rc != 0) { std::fprintf(stderr, "%s: directory %s missing (%d)\n", when, d.c_str(), rc); g_fail++; continue; }
    FsStat st;
    CHECK(v.stat(ino, &st) == 0 && st.is_dir());
  }
  for (auto& [path, data] : m) {
    uint64_t ino;
    int rc = v.resolve(path, &ino);
    if (rc != 0) { std::fprintf(stderr, "%s: %s missing (%d)\n", when, path.c_str(), rc); g_fail++; continue; }
    FsStat st;
    CHECK(v.stat(ino, &st) == 0);
    if (st.size != data.size()) { std::fprintf(stderr, "%s: %s size %llu expected %zu\n", when, path.c_str(), static_cast<unsigned long long>(st.size), data.size()); g_fail++; continue; }
    std::vector<uint8_t> back(data.size());
    size_t off = 0;
    while (off < back.size()) {
      int64_t r = v.read(ino, off, back.data() + off, back.size() - off);
      if (r <= 0) break;
      off += static_cast<size_t>(r);
    }
    if (off != back.size() || back != data) { std::fprintf(stderr, "%s: %s content differs\n", when, path.c_str()); g_fail++; }
  }
}

int main(int argc, char** argv) {
  if (argc < 3) { std::fprintf(stderr, "usage: %s <fs> <image> [--cluster N] [--label L] [--sector N] [--features F] [--ro]\n", argv[0]); return 64; }
  std::string fsn = argv[1];
  MkfsOptions mo;
  Kind kind;
  if (fsn == "ext2") { mo.fs = FsType::Ext2; kind = Kind::Ext2; }
  else if (fsn == "ext3") { mo.fs = FsType::Ext3; kind = Kind::Ext4; }
  else if (fsn == "ext4") { mo.fs = FsType::Ext4; kind = Kind::Ext4; }
  else if (fsn == "fat12") { mo.fs = FsType::Fat12; kind = Kind::Fat; }
  else if (fsn == "fat16") { mo.fs = FsType::Fat16; kind = Kind::Fat; }
  else if (fsn == "fat32") { mo.fs = FsType::Fat32; kind = Kind::Fat; }
  else if (fsn == "exfat") { mo.fs = FsType::Exfat; kind = Kind::Exfat; }
  else if (fsn == "ntfs") { mo.fs = FsType::Ntfs; kind = Kind::Ntfs; }
  else { std::fprintf(stderr, "unknown fs %s\n", fsn.c_str()); return 64; }
  bool ro_only = false;
  uint32_t bs = 512;
  for (int i = 3; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--cluster" && i + 1 < argc) mo.cluster_size = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0));
    else if (a == "--label" && i + 1 < argc) mo.label = argv[++i];
    else if (a == "--sector" && i + 1 < argc) { mo.sector_size = static_cast<uint32_t>(std::strtoul(argv[++i], nullptr, 0)); bs = mo.sector_size; }
    else if (a == "--features" && i + 1 < argc) mo.features = argv[++i];
    else if (a == "--ro") ro_only = true;
  }
  mo.seed = 12345;

  // ---- mkfs
  {
    BlockDevicePtr dev;
    if (!RawImageDevice::open_path(argv[2], false, bs, &dev).ok()) { std::fprintf(stderr, "cannot open the image\n"); return 1; }
    MkfsPlan plan;
    MkfsResult pr = Mkfs::plan(*dev, mo, &plan);
    if (!pr.ok() || !plan.ok) { std::fprintf(stderr, "mkfs plan failed: %s %s\n", pr.code.c_str(), pr.message.c_str()); return 1; }
    MkfsApplyOptions ao;
    ao.backup_sink = [](const std::vector<uint8_t>&) { return Status::Ok(); };
    MkfsResult r = Mkfs::apply(*dev, plan, Mkfs::make_confirm_token(plan, plan.confirm_phrase), ao);
    if (!r.ok() || !r.verified) { std::fprintf(stderr, "mkfs apply failed: %s %s\n", r.code.c_str(), r.message.c_str()); return 1; }
  }

  // ---- 読み書き
  Model model;
  std::vector<std::string> dirs;
  if (!ro_only) {
    BlockDevicePtr dev;
    RawImageDevice::open_path(argv[2], false, bs, &dev);
    int err = 0;
    auto v = mount(kind, dev, true, &err);
    if (!v) {
      if (err == -EROFS || err == EROFS) { std::fprintf(stderr, "SKIP: the build has no write support\n"); return 77; }
      std::fprintf(stderr, "rw mount failed: %d\n", err);
      return 1;
    }
    if (!v->writable()) { std::fprintf(stderr, "mounted read-only (state 0x%x)\n", v->state()); return 1; }
    uint64_t root = v->root(), d1 = 0, d2 = 0, ino = 0;
    CHECK_RC(v->mkdir(root, "docs", 0755, &d1));
    CHECK_RC(v->mkdir(d1, "sub dir", 0755, &d2));
    dirs = {"/docs", "/docs/sub dir"};
    const size_t big = std::min<uint64_t>(3000000, dev->size() / 8);   // 小さいボリューム (フロッピー相当) でも入る大きさ
    struct F { uint64_t dir; std::string dirpath, name; size_t size; unsigned seed; };
    std::vector<F> files = {{root, "", "small.txt", 100, 1}, {d1, "/docs", "medium.bin", 70000, 2}, {d2, "/docs/sub dir", "big.bin", big, 3},
                            {root, "", "Long File Name with spaces.dat", 5000, 4}, {d1, "/docs", "zero.bin", 0, 5}};
    for (int i = 0; i < 40; i++) files.push_back({d1, "/docs", "f" + std::to_string(i) + ".t", static_cast<size_t>(i * 37 + 1), static_cast<unsigned>(i + 10)});
    for (auto& f : files) {
      CHECK_RC(v->create(f.dir, f.name, 0644, &ino));
      auto data = pattern(f.size, f.seed);
      size_t off = 0;
      while (off < data.size()) {
        size_t n = std::min<size_t>(data.size() - off, 65536);
        int64_t w = v->write(ino, off, data.data() + off, n);
        if (w <= 0) { std::fprintf(stderr, "write %s failed: %lld\n", f.name.c_str(), static_cast<long long>(w)); g_fail++; break; }
        off += static_cast<size_t>(w);
      }
      model[f.dirpath + "/" + f.name] = data;
    }
    // 追記
    {
      uint64_t i2;
      CHECK_RC(v->lookup(d1, "medium.bin", &i2));
      auto extra = pattern(40000, 99);
      CHECK(v->write(i2, 70000, extra.data(), extra.size()) == static_cast<int64_t>(extra.size()));
      auto& m = model["/docs/medium.bin"];
      m.insert(m.end(), extra.begin(), extra.end());
    }
    // 改名と削除
    CHECK_RC(v->rename(d1, "f3.t", d2, "moved.t"));
    model["/docs/sub dir/moved.t"] = model["/docs/f3.t"];
    model.erase("/docs/f3.t");
    for (int i = 20; i < 30; i++) {
      CHECK_RC(v->unlink(d1, "f" + std::to_string(i) + ".t"));
      model.erase("/docs/f" + std::to_string(i) + ".t");
    }
    CHECK_RC(v->sync());
    verify_tree(*v, model, dirs, "after writes");
    v.reset();
  }

  // ---- 読み取り専用で再マウントして突き合わせ
  {
    BlockDevicePtr dev;
    RawImageDevice::open_path(argv[2], true, bs, &dev);
    int err = 0;
    auto v = mount(kind, dev, false, &err);
    if (!v) { std::fprintf(stderr, "ro mount failed: %d\n", err); return 1; }
    verify_tree(*v, model, dirs, "after remount");
    std::vector<FsDirEntry> ents;
    CHECK(v->readdir(v->root(), &ents) == 0);
  }

  // ---- 自前の検査
  {
    BlockDevicePtr dev;
    RawImageDevice::open_path(argv[2], true, bs, &dev);
    FsCheckReport rep;
    Status st = check_filesystem(*dev, 0, dev->size(), mo.fs, FsCheckOptions{}, &rep);
    if (!st.ok() || (rep.verdict != FsVerdict::Clean && rep.verdict != FsVerdict::Incomplete)) {
      std::fprintf(stderr, "our check after the round trip: verdict %d\n", static_cast<int>(rep.verdict));
      for (auto& f : rep.findings) std::fprintf(stderr, "  %s %s\n", f.code.c_str(), f.message.c_str());
      g_fail++;
    }
  }
  if (g_fail) { std::fprintf(stderr, "FAILED (%d)\n", g_fail); return 1; }
  std::printf("roundtrip ok: %s\n", fsn.c_str());
  return 0;
}
