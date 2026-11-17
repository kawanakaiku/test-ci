// SPDX-License-Identifier: GPL-2.0-or-later
// fsdump: NTFS イメージのツリー (名前・種別・サイズ・mtime・sha256) を書き出す。性能の測定も同じ実行体でする。docs/fuse-layer.md 5 章
//   - 既定のビルド (USBMEM_FSDUMP_NTFS3G なし): 自前の NTFS 実装 (LinuxCompat 上の fs/ntfs = NtfsVolume)
//   - USBMEM_FSDUMP_NTFS3G: shim 越しの無改変の ntfs-3g (FuseShimVolume)
// 2 つは 1 つの実行体にできない (どちらも ntfs_* のシンボルを定義する)。別プロセスで動かし、出力を diff で突き合わせる (tools/run_fuseshim_tests.sh)。
//
//   fsdump IMAGE                        ツリーを標準出力へ (行: 種別 サイズ mtime sha256 パス。種別 d / f / l / o)。統計は標準エラーへ
//   fsdump --perf IMAGE BIGFILE MANYDIR  性能 (key=value を標準出力へ)。BIGFILE / MANYDIR はルートからのパス (例 /big.bin /many)
#include <sys/resource.h>
#include <sys/stat.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "usbmem/devices.h"
#include "usbmem/fs_volume.h"
#include "usbmem/sha256.h"
#ifdef USBMEM_FSDUMP_NTFS3G
#include "ntfs3g_session.h"
#else
#include "usbmem/ntfs_volume.h"
#endif

using namespace usbmem;
using Clock = std::chrono::steady_clock;

namespace {

// 下位デバイスへの読み出し回数とバイト数を数える (余計な I/O の有無を見る)
class CountingDevice final : public BlockDevice {
 public:
  explicit CountingDevice(BlockDevicePtr in) : in_(std::move(in)) {}
  Status read_at(uint64_t off, void* buf, size_t len, const IoOptions& o = {}) override {
    calls_++;
    bytes_ += len;
    return in_->read_at(off, buf, len, o);
  }
  Status write_at(uint64_t off, const void* buf, size_t len, const IoOptions& o = {}) override { return in_->write_at(off, buf, len, o); }
  Status flush() override { return in_->flush(); }
  Status close() override { return in_->close(); }
  uint64_t size() const override { return in_->size(); }
  uint32_t logical_block_size() const override { return in_->logical_block_size(); }
  bool is_read_only() const override { return in_->is_read_only(); }
  std::string describe() const override { return "counting:" + in_->describe(); }
  uint64_t calls() const { return calls_; }
  uint64_t bytes() const { return bytes_; }

 private:
  BlockDevicePtr in_;
  std::atomic<uint64_t> calls_{0}, bytes_{0};
};

struct Opened {
  std::shared_ptr<CountingDevice> dev;
#ifdef USBMEM_FSDUMP_NTFS3G
  std::unique_ptr<FuseShimSession> session;
  FsVolume* vol() { return session ? session->volume() : nullptr; }
#else
  std::unique_ptr<NtfsVolume> v;
  FsVolume* vol() { return v.get(); }
#endif
};

bool open_image(const std::string& path, Opened* o, double* mount_ms) {
  BlockDevicePtr raw;
  if (!RawImageDevice::open_path(path, true, 512, &raw).ok()) {
    std::fprintf(stderr, "fsdump: cannot open %s\n", path.c_str());
    return false;
  }
  o->dev = std::make_shared<CountingDevice>(raw);
  auto t0 = Clock::now();
#ifdef USBMEM_FSDUMP_NTFS3G
  std::string err;
  o->session = open_ntfs3g(o->dev, "", &err);
  if (!o->session) {
    std::fprintf(stderr, "fsdump: ntfs3g mount failed: %s\n", err.c_str());
    return false;
  }
#else
  int e = 0;
  o->v = NtfsVolume::mount(o->dev, &e);
  if (!o->v) {
    std::fprintf(stderr, "fsdump: mount failed errno %d\n", e);
    return false;
  }
#endif
  *mount_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
  return true;
}

bool sha_of(FsVolume& v, uint64_t ino, uint64_t size, std::string* hex, std::vector<uint8_t>& buf) {
  Sha256 h;
  uint64_t off = 0;
  for (;;) {
    int64_t n = v.read(ino, off, buf.data(), buf.size());
    if (n < 0) return false;
    if (n == 0) break;
    h.update(buf.data(), static_cast<size_t>(n));
    off += static_cast<uint64_t>(n);
  }
  (void)size;
  *hex = hex_encode(h.finish());
  return true;
}

struct Totals {
  uint64_t dirs = 0, files = 0, links = 0, other = 0, bytes = 0, errors = 0;
};

void walk(FsVolume& v, uint64_t dir, const std::string& path, Totals* t, std::vector<uint8_t>& buf) {
  std::vector<FsDirEntry> ents;
  int rc = v.readdir(dir, &ents);
  if (rc) {
    std::printf("ERR readdir %s %d\n", path.empty() ? "/" : path.c_str(), rc);
    t->errors++;
    return;
  }
  std::sort(ents.begin(), ents.end(), [](const FsDirEntry& a, const FsDirEntry& b) { return a.name < b.name; });
  for (const auto& e : ents) {
    if (e.name == "." || e.name == "..") continue;
    std::string p = path + "/" + e.name;
    FsStat st;
    rc = v.stat(e.ino, &st);
    if (rc) {
      std::printf("ERR stat %s %d\n", p.c_str(), rc);
      t->errors++;
      continue;
    }
    switch (st.mode & S_IFMT) {
      case S_IFDIR:
        t->dirs++;
        std::printf("d - %lld - %s\n", static_cast<long long>(st.mtime), p.c_str());
        walk(v, e.ino, p, t, buf);
        break;
      case S_IFREG: {
        std::string hex;
        if (!sha_of(v, e.ino, st.size, &hex, buf)) {
          std::printf("ERR read %s\n", p.c_str());
          t->errors++;
          break;
        }
        t->files++;
        t->bytes += st.size;
        std::printf("f %llu %lld %s %s\n", static_cast<unsigned long long>(st.size), static_cast<long long>(st.mtime), hex.c_str(), p.c_str());
        break;
      }
      case S_IFLNK: {
        std::string target;
        rc = v.readlink(e.ino, &target);
        t->links++;
        std::printf("l - %lld %s %s\n", static_cast<long long>(st.mtime), rc ? "ERR" : target.c_str(), p.c_str());
        break;
      }
      default:
        t->other++;
        std::printf("o - %lld - %s\n", static_cast<long long>(st.mtime), p.c_str());
    }
  }
}

int resolve_path(FsVolume& v, const std::string& path, uint64_t* ino) {
  uint64_t cur = v.root();
  size_t i = 0;
  while (i < path.size()) {
    while (i < path.size() && path[i] == '/') i++;
    size_t j = path.find('/', i);
    if (j == std::string::npos) j = path.size();
    if (j > i) {
      int rc = v.lookup(cur, path.substr(i, j - i), &cur);
      if (rc) return rc;
    }
    i = j;
  }
  *ino = cur;
  return 0;
}

double ms_since(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

int perf(const std::string& image, const std::string& big, const std::string& many) {
  Opened o;
  double mount_ms = 0;
  if (!open_image(image, &o, &mount_ms)) return 2;
  FsVolume& v = *o.vol();
  std::printf("perf.mount_ms=%.1f\n", mount_ms);

  uint64_t ino = 0;
  FsStat st;
  if (resolve_path(v, big, &ino) || v.stat(ino, &st)) {
    std::fprintf(stderr, "fsdump: %s not found\n", big.c_str());
    return 2;
  }
  std::vector<uint8_t> buf(1u << 20);
  std::vector<double> mbps;
  uint64_t calls0 = o.dev->calls(), bytes0 = o.dev->bytes();
  for (int run = 0; run < 3; run++) {
    auto t0 = Clock::now();
    uint64_t off = 0;
    for (;;) {
      int64_t n = v.read(ino, off, buf.data(), buf.size());
      if (n < 0) {
        std::fprintf(stderr, "fsdump: read error %lld\n", static_cast<long long>(n));
        return 3;
      }
      if (n == 0) break;
      off += static_cast<uint64_t>(n);
    }
    double s = ms_since(t0) / 1000.0;
    mbps.push_back(static_cast<double>(off) / 1048576.0 / s);
    if (run == 0) {
      std::printf("perf.big_bytes=%llu\n", static_cast<unsigned long long>(off));
      std::printf("perf.read_dev_calls_first_run=%llu\n", static_cast<unsigned long long>(o.dev->calls() - calls0));
      std::printf("perf.read_dev_bytes_first_run=%llu\n", static_cast<unsigned long long>(o.dev->bytes() - bytes0));
    }
  }
  std::printf("perf.read_mbps_runs=%.0f,%.0f,%.0f\n", mbps[0], mbps[1], mbps[2]);
  std::printf("perf.read_mbps_best=%.0f\n", *std::max_element(mbps.begin(), mbps.end()));

  uint64_t dino = 0;
  if (resolve_path(v, many, &dino)) {
    std::fprintf(stderr, "fsdump: %s not found\n", many.c_str());
    return 2;
  }
  std::vector<FsDirEntry> ents;
  auto t1 = Clock::now();
  int rc = v.readdir(dino, &ents);
  double readdir_ms = ms_since(t1);
  if (rc) return 3;
  std::printf("perf.dir_entries=%zu\n", ents.size());
  std::printf("perf.readdir_ms=%.1f\n", readdir_ms);
  t1 = Clock::now();
  uint64_t sum = 0;
  for (const auto& e : ents) {
    FsStat s2;
    if (e.name == "." || e.name == "..") continue;
    if (v.stat(e.ino, &s2) == 0) sum += s2.size;
  }
  std::printf("perf.stat_all_ms=%.1f\n", ms_since(t1));
  t1 = Clock::now();
  uint64_t looked = 0;
  for (const auto& e : ents) {
    uint64_t x = 0;
    if (e.name == "." || e.name == "..") continue;
    if (v.lookup(dino, e.name, &x) == 0) looked++;
  }
  std::printf("perf.lookup_all_ms=%.1f\n", ms_since(t1));
  t1 = Clock::now();
  uint64_t small_bytes = 0;
  std::vector<uint8_t> sb(4096);
  for (const auto& e : ents) {
    if (e.name == "." || e.name == "..") continue;
    int64_t n = v.read(e.ino, 0, sb.data(), sb.size());
    if (n > 0) small_bytes += static_cast<uint64_t>(n);
  }
  std::printf("perf.read_all_small_ms=%.1f\n", ms_since(t1));
  std::printf("perf.small_bytes=%llu looked=%llu sizesum=%llu\n", static_cast<unsigned long long>(small_bytes), static_cast<unsigned long long>(looked), static_cast<unsigned long long>(sum));
  std::printf("perf.dev_calls_total=%llu\n", static_cast<unsigned long long>(o.dev->calls()));
  std::printf("perf.dev_bytes_total=%llu\n", static_cast<unsigned long long>(o.dev->bytes()));
  struct rusage ru;
  getrusage(RUSAGE_SELF, &ru);
  std::printf("perf.maxrss_kb=%ld\n", ru.ru_maxrss);
  return 0;
}

}  // namespace

int main(int argc, char** argv) {
  if (argc == 5 && !std::strcmp(argv[1], "--perf")) return perf(argv[2], argv[3], argv[4]);
  if (argc != 2) {
    std::fprintf(stderr, "usage: %s IMAGE | --perf IMAGE BIGFILE MANYDIR\n", argv[0]);
    return 64;
  }
  Opened o;
  double mount_ms = 0;
  if (!open_image(argv[1], &o, &mount_ms)) return 2;
  Totals t;
  std::vector<uint8_t> buf(1u << 20);
  auto t0 = Clock::now();
  walk(*o.vol(), o.vol()->root(), "", &t, buf);
  std::fprintf(stderr, "fsdump: mount %.1f ms, walk %.1f ms, dirs=%llu files=%llu links=%llu other=%llu bytes=%llu errors=%llu dev_calls=%llu dev_bytes=%llu\n", mount_ms, ms_since(t0),
               static_cast<unsigned long long>(t.dirs), static_cast<unsigned long long>(t.files), static_cast<unsigned long long>(t.links), static_cast<unsigned long long>(t.other),
               static_cast<unsigned long long>(t.bytes), static_cast<unsigned long long>(t.errors), static_cast<unsigned long long>(o.dev->calls()), static_cast<unsigned long long>(o.dev->bytes()));
  std::fflush(stdout);
  return t.errors ? 1 : 0;
}
