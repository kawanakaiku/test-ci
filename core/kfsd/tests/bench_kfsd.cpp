// SPDX-License-Identifier: GPL-2.0-or-later
// kfsd の性能の測定 (docs/kfsd.md 4 章): プロセス内のドライバと、RPC (別プロセスの kfsd) で、同じ操作の速度を比べる。
//   usbmem_kfsd_bench --kfsd <kfsd> <空の FS のイメージ> [--size-mb N] [--files N]
// イメージは mkfs したばかりの書き込み可能な FS (tools/run_kfsd_bench.sh が作る)。変種ごとに新しいコピーを使う。
// 出力は表 (stderr)。環境変数 GITHUB_STEP_SUMMARY があれば、同じ内容を markdown で追記する。
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "kfsd_mount.h"
#include "usbmem/devices.h"
#include "usbmem/kfsd_client.h"

using namespace usbmem;

namespace {

double now_s() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

uint64_t file_size(const std::string& p) { struct stat st; return ::stat(p.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_size) : 0; }

bool copy_sparse(const std::string& from, const std::string& to) {
  int in = ::open(from.c_str(), O_RDONLY | O_CLOEXEC), out = ::open(to.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  if (in < 0 || out < 0) return false;
  uint64_t size = file_size(from);
  if (::ftruncate(out, static_cast<off_t>(size)) != 0) return false;
  std::vector<uint8_t> buf(1 << 20);
  bool ok = true;
  for (uint64_t off = 0; off < size && ok; off += buf.size()) {
    ssize_t n = ::pread(in, buf.data(), buf.size(), static_cast<off_t>(off));
    if (n <= 0) { ok = false; break; }
    bool zero = true;
    for (ssize_t i = 0; i < n && zero; i++) if (buf[static_cast<size_t>(i)]) zero = false;
    if (!zero && ::pwrite(out, buf.data(), static_cast<size_t>(n), static_cast<off_t>(off)) != n) ok = false;
  }
  ::close(in);
  ::close(out);
  return ok;
}

struct Variant { const char* name; bool rpc; bool prefetch; };

struct Vol {
  std::unique_ptr<FsVolume> v;
  kfsd::RpcVolume* rpc = nullptr;
  int err = 0;
};

std::string g_kfsd;

Vol open_vol(const std::string& img, const Variant& var) {
  Vol o;
  BlockDevicePtr dev;
  if (!RawImageDevice::open_path(img, false, 512, &dev).ok()) { o.err = EIO; return o; }
  if (!var.rpc) {
    o.v = kfsd::mount_auto(dev, true, &o.err, false, true);
  } else {
    kfsd::MountOptions mo;
    mo.rw = true;
    mo.prefetch_stat = var.prefetch;
    auto r = kfsd::RpcVolume::mount(g_kfsd, dev, mo, &o.err);
    o.rpc = r.get();
    o.v = std::move(r);
  }
  return o;
}

struct Row { std::string what; double v; std::string unit; };

}  // namespace

int main(int argc, char** argv) {
  std::string img;
  int size_mb = 128, nfiles = 3000;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "--kfsd" && i + 1 < argc) g_kfsd = argv[++i];
    else if (a == "--size-mb" && i + 1 < argc) size_mb = std::atoi(argv[++i]);
    else if (a == "--files" && i + 1 < argc) nfiles = std::atoi(argv[++i]);
    else img = a;
  }
  if (g_kfsd.empty()) g_kfsd = kfsd::find_exe();
  if (img.empty() || g_kfsd.empty()) { std::fprintf(stderr, "usage: %s --kfsd <kfsd> <image> [--size-mb N] [--files N]\n", argv[0]); return 64; }
  std::string base = ::access("/dev/shm", W_OK) == 0 ? "/dev/shm" : "/tmp";
  std::string t = base + "/usbmem_kfsd_bXXXXXX";
  std::vector<char> tb(t.begin(), t.end());
  tb.push_back('\0');
  if (!::mkdtemp(tb.data())) return 1;
  std::string work = tb.data();
  const Variant variants[] = {{"inproc", false, false}, {"rpc", true, false}, {"rpc+prefetch", true, true}};
  std::vector<std::vector<Row>> results;
  std::string fsname;
  const size_t total = static_cast<size_t>(size_mb) << 20;
  std::vector<uint8_t> chunk(4 << 20);
  for (size_t i = 0; i < chunk.size(); i++) chunk[i] = static_cast<uint8_t>(i * 131 + (i >> 9));

  for (const auto& var : variants) {
    std::string copy = work + "/bench.img";
    if (!copy_sparse(img, copy)) { std::fprintf(stderr, "copy failed\n"); return 1; }
    std::vector<Row> rows;
    uint64_t file = 0, dir = 0;
    {
      Vol o = open_vol(copy, var);
      if (!o.v) { std::fprintf(stderr, "mount failed (%s): err=%d\n", var.name, o.err); return 1; }
      fsname = o.v->fs_name();
      if (!o.v->writable()) { std::fprintf(stderr, "not writable: %s\n", fsname.c_str()); return 77; }
      FsVolume& v = *o.v;
      // ---- 連続書き込み (1 MiB ずつ)
      if (v.create(v.root(), "big.bin", 0644, &file) != 0) { std::fprintf(stderr, "create failed\n"); return 1; }
      double t0 = now_s();
      for (size_t off = 0; off < total; off += (1 << 20)) {
        size_t n = std::min<size_t>(1 << 20, total - off);
        if (v.write(file, off, chunk.data(), n) != static_cast<int64_t>(n)) { std::fprintf(stderr, "write failed at %zu\n", off); return 1; }
      }
      v.sync();
      double dt = now_s() - t0;
      rows.push_back({"seq write 1 MiB", size_mb / dt, "MB/s"});
      // ---- 大きな 1 回の書き込み (4 MiB)
      uint64_t f2 = 0;
      v.create(v.root(), "big2.bin", 0644, &f2);
      t0 = now_s();
      for (size_t off = 0; off < total; off += (4 << 20)) {
        size_t n = std::min<size_t>(4 << 20, total - off);
        if (v.write(f2, off, chunk.data(), n) != static_cast<int64_t>(n)) { std::fprintf(stderr, "write2 failed\n"); return 1; }
      }
      v.sync();
      rows.push_back({"seq write 4 MiB", size_mb / (now_s() - t0), "MB/s"});
      // ---- メタデータ: 多数の小さなファイルを作る
      v.mkdir(v.root(), "many", 0755, &dir);
      t0 = now_s();
      int made = 0;
      for (int i = 0; i < nfiles; i++) {
        char nm[64];
        std::snprintf(nm, sizeof nm, "file-%06d.txt", i);
        uint64_t ino;
        if (v.create(dir, nm, 0644, &ino) != 0) break;
        made++;
      }
      v.sync();
      rows.push_back({"create (empty file)", (now_s() - t0) * 1e6 / std::max(made, 1), "us/op"});
      if (var.rpc) {
        uint64_t rc, rb, wc, wb;
        o.rpc->dev_stats(&rc, &rb, &wc, &wb);
        rows.push_back({"device writes (write phase)", static_cast<double>(wc), "calls"});
        rows.push_back({"device write avg size", wc ? static_cast<double>(wb) / static_cast<double>(wc) / 1024 : 0, "KiB"});
      }
    }
    // 読み: 読むサイズごとに再マウントして (LinuxCompat のキャッシュを空にして) big.bin を先頭から読む。4 KiB は往復が多いので 32 MiB まで
    for (size_t bs : {size_t(4096), size_t(65536), size_t(1) << 20, size_t(4) << 20}) {
      Vol o = open_vol(copy, var);
      if (!o.v) { std::fprintf(stderr, "remount failed\n"); return 1; }
      FsVolume& v = *o.v;
      uint64_t ino = 0;
      v.lookup(v.root(), "big.bin", &ino);
      std::vector<uint8_t> buf(4 << 20);
      size_t amount = bs == 4096 ? std::min<size_t>(total, 32 << 20) : total;
      double t0 = now_s();
      size_t done = 0;
      for (size_t off = 0; off < amount; off += bs) {
        int64_t r = v.read(ino, off, buf.data(), bs);
        if (r <= 0) break;
        done += static_cast<size_t>(r);
      }
      char nm[64];
      std::snprintf(nm, sizeof nm, "seq read %zu KiB (cold)", bs >> 10);
      rows.push_back({nm, done / 1048576.0 / (now_s() - t0), "MB/s"});
      if (bs == (1u << 20)) {   // 同じ範囲をもう一度 (ドライバのキャッシュに載っている: 往復だけの速さ)
        t0 = now_s();
        done = 0;
        for (size_t off = 0; off < amount; off += bs) {
          int64_t r = v.read(ino, off, buf.data(), bs);
          if (r <= 0) break;
          done += static_cast<size_t>(r);
        }
        rows.push_back({"seq read 1024 KiB (warm)", done / 1048576.0 / (now_s() - t0), "MB/s"});
        if (o.rpc) {
          uint64_t rc, rb, wc, wb;
          o.rpc->dev_stats(&rc, &rb, &wc, &wb);
          rows.push_back({"device reads (1 MiB cold+warm)", static_cast<double>(rc), "calls"});
          rows.push_back({"device read avg size", rc ? static_cast<double>(rb) / static_cast<double>(rc) / 1024 : 0, "KiB"});
        }
      }
    }
    // メタデータ (再マウント直後 = 冷えた状態)
    {
      Vol o = open_vol(copy, var);
      if (!o.v) { std::fprintf(stderr, "remount failed\n"); return 1; }
      FsVolume& v = *o.v;
      uint64_t dir2 = 0;
      v.lookup(v.root(), "many", &dir2);
      std::vector<FsDirEntry> ents;
      double t0 = now_s();
      v.readdir(dir2, &ents);
      double t_rd = now_s() - t0;
      rows.push_back({"readdir (whole dir)", t_rd * 1e3, "ms"});
      t0 = now_s();
      for (auto& e : ents) { FsStat st; v.stat(e.ino, &st); }
      double t_st = now_s() - t0;
      rows.push_back({"stat after readdir", t_st * 1e6 / std::max<size_t>(ents.size(), 1), "us/op"});
      rows.push_back({"readdir + stat all", (t_rd + t_st) * 1e3, "ms"});
      t0 = now_s();
      for (auto& e : ents) { uint64_t i2; v.lookup(dir2, e.name, &i2); }
      rows.push_back({"lookup (by name, hot)", (now_s() - t0) * 1e6 / std::max<size_t>(ents.size(), 1), "us/op"});
      FsStat st;   // 1 往復の最小の遅延: 同じ ino の stat を繰り返す
      t0 = now_s();
      const int N = 20000;
      for (int i = 0; i < N; i++) v.stat(dir2, &st);
      rows.push_back({"stat (same ino, hot)", (now_s() - t0) * 1e6 / N, "us/op"});
      if (o.rpc) rows.push_back({"stat cache hits", static_cast<double>(o.rpc->stat_cache_hits()), "count"});
    }
    ::unlink(copy.c_str());
    results.push_back(std::move(rows));
  }
  ::rmdir(work.c_str());

  // ---- 表
  std::string md = "#### kfsd benchmark: " + fsname + " (file " + std::to_string(size_mb) + " MiB, " + std::to_string(nfiles) + " files)\n\n| metric | unit | inproc | rpc | rpc+prefetch |\n|---|---|---|---|---|\n";
  std::fprintf(stderr, "\n=== kfsd benchmark: %s (file %d MiB, %d files) ===\n%-36s %-8s %12s %12s %14s\n", fsname.c_str(), size_mb, nfiles, "metric", "unit", "inproc", "rpc", "rpc+prefetch");
  for (size_t i = 0; i < results[0].size(); i++) {
    auto val = [&](size_t k) -> std::string {
      if (i < results[k].size() && results[k][i].what == results[0][i].what) { char b[32]; std::snprintf(b, sizeof b, "%.1f", results[k][i].v); return b; }
      for (auto& r : results[k]) if (r.what == results[0][i].what) { char b[32]; std::snprintf(b, sizeof b, "%.1f", r.v); return b; }
      return "-";
    };
    std::fprintf(stderr, "%-36s %-8s %12s %12s %14s\n", results[0][i].what.c_str(), results[0][i].unit.c_str(), val(0).c_str(), val(1).c_str(), val(2).c_str());
    md += "| " + results[0][i].what + " | " + results[0][i].unit + " | " + val(0) + " | " + val(1) + " | " + val(2) + " |\n";
  }
  // rpc 専用の行 (inproc に無い) も出す
  for (size_t k = 1; k < results.size(); k++)
    for (auto& r : results[k]) {
      bool in0 = false;
      for (auto& r0 : results[0]) if (r0.what == r.what) in0 = true;
      if (!in0 && k == 1) {
        char b[32];
        std::snprintf(b, sizeof b, "%.1f", r.v);
        std::fprintf(stderr, "%-36s %-8s %12s %12s %14s\n", r.what.c_str(), r.unit.c_str(), "-", b, "");
        md += "| " + r.what + " | " + r.unit + " | - | " + b + " | |\n";
      }
    }
  if (const char* sum = std::getenv("GITHUB_STEP_SUMMARY")) {
    if (FILE* f = std::fopen(sum, "a")) { std::fputs((md + "\n").c_str(), f); std::fclose(f); }
  }
  return 0;
}
