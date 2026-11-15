// SPDX-License-Identifier: Apache-2.0
// 書き込みの I/O 形 (回数・大きさ・順序・flush) を段階ごとに数えるベンチ (USBMEM_NTFS_WRITE=ON のビルド用)。
// 実機の RealUsbWriteTest (CreateOnly → Full、32 MiB の書き込み) と同じ操作列を、ブロックデバイス層で数える。docs/real-usb-testset.md 10.4
//   bench_write_io <f2fs|btrfs> <img> [stage...]     stage: co (CreateOnly 相当) / full (Full 相当: 前段のフォルダを消して 32 MiB)。段階ごとに再マウント。
// 出力: 段階 × 手順ごとの 書き込み回数 / バイト / 小さい書き込み (< 64 KiB) / 非連続 (直前の終端と違う場所) の回数 / flush 回数 / 読み出し回数・バイト / 経過 ms
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "usbmem/btrfs_volume.h"
#include "usbmem/devices.h"
#include "usbmem/f2fs_volume.h"

using namespace usbmem;

namespace {
struct Counters {
  uint64_t w = 0, wbytes = 0, wsmall = 0, wjump = 0, flushes = 0, r = 0, rbytes = 0, rsmall = 0;
  uint64_t wmax = 0;
};

class CountDev : public BlockDevice {
 public:
  explicit CountDev(BlockDevicePtr p) : p_(std::move(p)) {}
  Status read_at(uint64_t off, void* buf, size_t len, const IoOptions& o = {}) override {
    c.r++; c.rbytes += len; if (len < 65536) c.rsmall++;
    return p_->read_at(off, buf, len, o);
  }
  Status write_at(uint64_t off, const void* buf, size_t len, const IoOptions& o = {}) override {
    c.w++; c.wbytes += len; if (len < 65536) c.wsmall++;
    if (len > c.wmax) c.wmax = len;
    if (off != next_) c.wjump++;
    next_ = off + len;
    if (trace_) std::fprintf(stderr, "W off=%llu len=%zu\n", (unsigned long long)off, len);
    return p_->write_at(off, buf, len, o);
  }
  Status flush() override { c.flushes++; if (trace_) std::fprintf(stderr, "F\n"); return p_->flush(); }
  Status close() override { return p_->close(); }
  uint64_t size() const override { return p_->size(); }
  uint32_t logical_block_size() const override { return p_->logical_block_size(); }
  bool is_read_only() const override { return false; }
  std::string describe() const override { return "count(" + p_->describe() + ")"; }
  Counters c;
  bool trace_ = std::getenv("BENCH_TRACE") != nullptr;
 private:
  BlockDevicePtr p_;
  uint64_t next_ = ~0ull;
};

using Clock = std::chrono::steady_clock;
struct Phase {
  CountDev* dev; std::string stage; Clock::time_point t0;
  void begin() { dev->c = Counters(); t0 = Clock::now(); }
  void end(const char* name) {
    double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    const Counters& c = dev->c;
    std::printf("%-5s %-34s %8.0f ms  W=%llu (%llu KiB, small=%llu, jump=%llu, max=%llu KiB) F=%llu  R=%llu (%llu KiB, small=%llu)\n", stage.c_str(), name, ms,
                (unsigned long long)c.w, (unsigned long long)(c.wbytes >> 10), (unsigned long long)c.wsmall, (unsigned long long)c.wjump,
                (unsigned long long)(c.wmax >> 10), (unsigned long long)c.flushes, (unsigned long long)c.r, (unsigned long long)(c.rbytes >> 10),
                (unsigned long long)c.rsmall);
  }
};

std::vector<uint8_t> pattern(size_t n, int seed) {
  std::vector<uint8_t> b(n);
  for (size_t i = 0; i < n; i++) b[i] = static_cast<uint8_t>((i * 7 + seed) & 255);
  return b;
}
}  // namespace

#define OK(x) do { int _r = (x); if (_r) { std::fprintf(stderr, "FAIL %s:%d: %s -> %d\n", __FILE__, __LINE__, #x, _r); return 1; } } while (0)

// NtfsVolume.create 相当 (作って sync)
static int mk(FsVolume& v, uint64_t dir, const std::string& name, bool isdir, uint64_t* ino) {
  int rc = isdir ? v.mkdir(dir, name, 0755, ino) : v.create(dir, name, 0644, ino);
  if (rc) return rc;
  return v.sync();
}
static int write_all(FsVolume& v, uint64_t ino, const std::vector<uint8_t>& d, size_t chunk) {
  for (size_t off = 0; off < d.size();) {
    size_t n = std::min(chunk, d.size() - off);
    int64_t w = v.write(ino, off, d.data() + off, n);
    if (w <= 0) return w < 0 ? static_cast<int>(-w) : 5;
    off += static_cast<size_t>(w);
  }
  return 0;
}
static int rm_tree(FsVolume& v, uint64_t dir, const std::string& name) {
  uint64_t ino;
  OK(v.lookup(dir, name, &ino));
  std::vector<FsDirEntry> ents;
  OK(v.readdir(ino, &ents));
  for (auto& e : ents) {
    if (e.name == "." || e.name == "..") continue;
    FsStat st; OK(v.stat(e.ino, &st));
    if ((st.mode & 0170000) == 0040000) { if (int r = rm_tree(v, ino, e.name)) return r; }
    else OK(v.unlink(ino, e.name));
  }
  return v.rmdir(dir, name);
}

int main(int argc, char** argv) {
  if (argc < 4) { std::fprintf(stderr, "usage: %s <f2fs|btrfs> <img> <co|full>...\n", argv[0]); return 2; }
  std::string fs = argv[1];
  for (int si = 3; si < argc; si++) {
    std::string stage = argv[si];
    BlockDevicePtr raw;
    if (!RawImageDevice::open_path(argv[2], false, 512, &raw).ok()) { std::fprintf(stderr, "open failed\n"); return 1; }
    auto cd = std::make_shared<CountDev>(raw);
    Phase ph{cd.get(), stage, {}};
    std::unique_ptr<FsVolume> v;
    int err = 0;
    ph.begin();
    if (fs == "f2fs") v = F2fsVolume::mount_rw(cd, &err);
    else v = BtrfsVolume::mount_rw(cd, &err);
    if (!v || !v->writable()) { std::fprintf(stderr, "mount_rw failed (%d)\n", err); return 1; }
    ph.end("mount");
    uint64_t root = v->root(), dir, ino;
    char name[64];
    std::snprintf(name, sizeof name, "usbmem_cotest_%s_%d", stage.c_str(), si);
    std::string folder = name;
    if (stage == "full") {
      // 前の段階のフォルダを消す (この試験が作ったもの)
      ph.begin();
      for (int pj = 3; pj < si; pj++) {
        char pn[64]; std::snprintf(pn, sizeof pn, "usbmem_cotest_%s_%d", argv[pj], pj);
        uint64_t x;
        if (v->lookup(root, pn, &x) == 0) { OK(rm_tree(*v, root, pn)); }
      }
      OK(v->sync());
      ph.end("前段のフォルダを消す+sync");
    }
    ph.begin(); OK(mk(*v, root, folder, true, &dir)); ph.end("フォルダを作る");
    ph.begin(); OK(mk(*v, dir, "f.bin", false, &ino)); OK(write_all(*v, ino, pattern(300000, 5), 65536)); OK(v->sync()); ph.end("300 KB");
    ph.begin();
    uint64_t sub; OK(mk(*v, dir, "sub", true, &sub));
    for (int i = 0; i < 10; i++) { uint64_t x; OK(mk(*v, sub, "s" + std::to_string(i) + ".txt", false, &x)); OK(write_all(*v, x, pattern(100 + i * 37, i), 65536)); }
    OK(v->sync()); ph.end("小ファイル 10 個");
    // 32 MiB: create (+sync) → 1 MiB ずつ write → sync
    ph.begin(); OK(mk(*v, dir, "speed.bin", false, &ino)); ph.end("32MiB: create+sync");
    ph.begin();
    auto chunk = pattern(1 << 20, 11);
    for (int i = 0; i < 32; i++) {
      int64_t w = v->write(ino, static_cast<uint64_t>(i) << 20, chunk.data(), chunk.size());
      if (w != (1 << 20)) { std::fprintf(stderr, "short write %lld\n", (long long)w); return 1; }
    }
    ph.end("32MiB: write x32 (no sync)");
    ph.begin(); OK(v->sync()); ph.end("32MiB: sync");
    ph.begin();
    std::vector<uint8_t> rb(1 << 20);
    for (int i = 0; i < 32; i++) { if (v->read(ino, static_cast<uint64_t>(i) << 20, rb.data(), rb.size()) != (1 << 20)) return 1; }
    ph.end("32MiB: read-back");
    ph.begin(); OK(v->sync()); v.reset(); cd->close(); ph.end("sync+unmount");
  }
  return 0;
}
