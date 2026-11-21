// SPDX-License-Identifier: GPL-2.0-or-later
// kfsd (別プロセス + RPC) とプロセス内のドライバで、同じ結果になることの試験 (docs/kfsd.md)。
//   usbmem_kfsd_tests --kfsd <kfsd の実行ファイル> read <フィクスチャのディレクトリ>...   *.img を全部マウントし、ツリー・内容・エラーを比べる
//   usbmem_kfsd_tests --kfsd <...> write <フィクスチャのディレクトリ>...                   イメージをコピーし、同じ操作列をプロセス内 / RPC で流して、結果のツリーと各操作の戻り値を比べる
//   usbmem_kfsd_tests --kfsd <...> abnormal <フィクスチャのディレクトリ>                    kfsd の強制終了・EOF・本体の死亡・タイムアウト・デバイスの抜去・マウント失敗
// 終了コード: 0 成功 / 1 失敗 / 77 試験できるものが無かった (SKIP)
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <thread>
#include <vector>

#include "kfsd_mount.h"
#include "usbmem/devices.h"
#include "usbmem/kfsd_client.h"
#include "usbmem/sha256.h"

using namespace usbmem;

static int g_fail = 0, g_checked = 0;
static std::string g_kfsd;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { std::fprintf(stderr, "CHECK_EQ failed %s:%d: %s (%lld) != %s (%lld)\n", __FILE__, __LINE__, #a, va_, #b, vb_); g_fail++; } } while (0)

namespace {

double now_ms() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

struct Opened {
  std::unique_ptr<FsVolume> v;
  int err = 0;
  std::string fs_type;
  kfsd::RpcVolume* rpc = nullptr;   // v が RPC のときだけ
};

BlockDevicePtr open_dev(const std::string& path, bool rw) {
  BlockDevicePtr d;
  Status st = RawImageDevice::open_path(path, !rw, 512, &d);
  return st.ok() ? d : nullptr;
}

Opened open_inproc(const std::string& path, bool rw, bool allow_noload = false, bool allow_dirty = true) {
  Opened o;
  auto dev = open_dev(path, rw);
  if (!dev) { o.err = EIO; return o; }
  o.v = kfsd::mount_auto(dev, rw, &o.err, allow_noload, allow_dirty, &o.fs_type);
  if (!o.v && o.err <= 0) o.err = EIO;
  return o;
}

Opened open_rpc(const std::string& path, bool rw, bool allow_noload = false, bool allow_dirty = true, bool prefetch = false) {
  Opened o;
  auto dev = open_dev(path, rw);
  if (!dev) { o.err = EIO; return o; }
  kfsd::MountOptions mo;
  mo.rw = rw;
  mo.allow_noload = allow_noload;
  mo.allow_dirty = allow_dirty;
  mo.prefetch_stat = prefetch;
  auto r = kfsd::RpcVolume::mount(g_kfsd, dev, mo, &o.err);
  if (r) { o.fs_type = r->fs_type(); o.rpc = r.get(); o.v = std::move(r); }
  return o;
}

struct DumpStats { uint64_t entries = 0, bytes = 0; };

// ツリー全体を行にする: path|dtype|stat rc|size|mode|nlink[|mtime]|内容の sha256 (大きなファイルは先頭と末尾) |ランダム読み|リンク先。ino は含めない (exFAT などは実行時採番)。
void dump(FsVolume& v, uint64_t dir, const std::string& path, bool with_mtime, std::vector<std::string>* out, DumpStats* ds, int depth = 0) {
  std::vector<FsDirEntry> ents;
  int rc = v.readdir(dir, &ents);
  if (rc) { out->push_back(path + "|readdir=" + std::to_string(rc)); return; }
  std::sort(ents.begin(), ents.end(), [](const FsDirEntry& a, const FsDirEntry& b) { return a.name < b.name; });
  for (auto& e : ents) {
    ds->entries++;
    FsStat st;
    int sr = v.stat(e.ino, &st);
    std::string full = path + "/" + e.name;
    std::string line = full + "|" + std::to_string(e.dtype) + "|" + std::to_string(sr);
    if (sr == 0) {
      line += "|" + std::to_string(st.size) + "|" + std::to_string(st.mode) + "|" + std::to_string(st.nlink);
      if (with_mtime) line += "|" + std::to_string(st.mtime);
      uint32_t type = st.mode & 0170000;
      if (type == 0100000 && e.name != "." && e.name != "..") {
        Sha256 h;
        const uint64_t kCap = 64ull << 20;
        std::vector<uint8_t> buf(1 << 20);
        auto rd = [&](uint64_t off, size_t n) {
          int64_t r = v.read(e.ino, off, buf.data(), std::min(n, buf.size()));
          h.update(&r, sizeof r);
          if (r > 0) { h.update(buf.data(), static_cast<size_t>(r)); ds->bytes += static_cast<uint64_t>(r); }
          return r;
        };
        if (st.size <= kCap) {
          for (uint64_t off = 0; off < st.size;) { int64_t r = rd(off, buf.size()); if (r <= 0) break; off += static_cast<uint64_t>(r); }
        } else {
          rd(0, 4 << 20);
          rd(st.size - (4 << 20), 4 << 20);
        }
        // ランダムな範囲の読み (RPC の分割・短い読み・EOF をまたぐ読みの検査)
        if (st.size > 0) {
          rd(st.size / 3, 300000);
          rd(st.size - 1, 100);
          rd(st.size, 10);
          rd(st.size > 5000 ? st.size - 5000 : 0, 9000);
        }
        auto d = h.finish();
        char hx[17];
        std::snprintf(hx, sizeof hx, "%02x%02x%02x%02x%02x%02x%02x%02x", d[0], d[1], d[2], d[3], d[4], d[5], d[6], d[7]);
        line += std::string("|") + hx;
      } else if (type == 0120000) {
        std::string t;
        int lr = v.readlink(e.ino, &t);
        line += "|->" + std::to_string(lr) + ":" + t;
      }
    }
    out->push_back(line);
    if (sr == 0 && (st.mode & 0170000) == 040000 && e.name != "." && e.name != ".." && depth < 24 && ds->entries < 300000)
      dump(v, e.ino, full, with_mtime, out, ds, depth + 1);
  }
}

bool same(const std::vector<std::string>& a, const std::vector<std::string>& b, const std::string& what) {
  if (a == b) return true;
  std::fprintf(stderr, "  DIFFERENT: %s (%zu vs %zu lines)\n", what.c_str(), a.size(), b.size());
  int shown = 0;
  for (size_t i = 0; i < std::max(a.size(), b.size()) && shown < 8; i++) {
    const std::string x = i < a.size() ? a[i] : "-", y = i < b.size() ? b[i] : "-";
    if (x != y) { std::fprintf(stderr, "    [%zu] inproc: %s\n    [%zu] rpc:    %s\n", i, x.c_str(), i, y.c_str()); shown++; }
  }
  return false;
}

std::vector<std::string> list_images(const std::string& dir) {
  std::vector<std::string> r;
  DIR* d = ::opendir(dir.c_str());
  if (!d) return r;
  while (dirent* e = ::readdir(d)) {
    std::string n = e->d_name;
    if (n.size() > 4 && n.compare(n.size() - 4, 4, ".img") == 0) r.push_back(dir + "/" + n);
  }
  ::closedir(d);
  std::sort(r.begin(), r.end());
  return r;
}
uint64_t file_size(const std::string& p) { struct stat st; return ::stat(p.c_str(), &st) == 0 ? static_cast<uint64_t>(st.st_size) : 0; }

// ---------------------------------------------------------------------------------------------------------------------------------
// read
void compare_mount(const std::string& img, bool noload, double* t_in, double* t_rpc, uint64_t* nent) {
  Opened a = open_inproc(img, false, noload);
  Opened b = open_rpc(img, false, noload);
  if (a.err != b.err || (a.v == nullptr) != (b.v == nullptr)) {
    std::fprintf(stderr, "  %s noload=%d: mount differs: inproc err=%d, rpc err=%d\n", img.c_str(), noload, a.err, b.err);
    g_fail++;
    return;
  }
  g_checked++;
  if (!a.v) { std::fprintf(stderr, "  %-60s noload=%d mount failed both: err=%d\n", img.c_str(), noload, a.err); return; }
  CHECK(std::string(a.v->fs_name()) == b.v->fs_name());
  CHECK_EQ(a.v->root(), b.v->root());
  CHECK_EQ(a.v->writable(), b.v->writable());
  CHECK_EQ(a.v->state(), b.v->state());
  CHECK(a.fs_type == b.fs_type);
  std::vector<std::string> la, lb, lc;
  DumpStats sa, sb, sc;
  double t0 = now_ms();
  dump(*a.v, a.v->root(), "", true, &la, &sa);
  double t1 = now_ms();
  dump(*b.v, b.v->root(), "", true, &lb, &sb);
  double t2 = now_ms();
  *t_in += t1 - t0;
  *t_rpc += t2 - t1;
  *nent += sa.entries;
  if (!same(la, lb, img + " tree")) g_fail++;
  // prefetch_stat (readdir に stat を同梱) でも同じ
  Opened c = open_rpc(img, false, noload, true, true);
  CHECK(c.v != nullptr);
  if (c.v) {
    dump(*c.v, c.v->root(), "", true, &lc, &sc);
    if (!same(la, lc, img + " tree (prefetch_stat)")) g_fail++;
  }
  uint64_t rc, rb, wc, wb;
  b.rpc->dev_stats(&rc, &rb, &wc, &wb);
  CHECK_EQ(wc, 0);   // 読み取り専用のマウントは、デバイスへ 1 バイトも書かない
  std::fprintf(stderr, "  %-60s %-8s entries=%-6llu inproc=%7.1fms rpc=%7.1fms dev_reads=%llu (%llu KiB)\n", img.c_str(), a.fs_type.c_str(),
               (unsigned long long)sa.entries, t1 - t0, t2 - t1, (unsigned long long)rc, (unsigned long long)(rb >> 10));
}

int run_read(const std::vector<std::string>& dirs) {
  double t_in = 0, t_rpc = 0;
  uint64_t nent = 0;
  int n = 0;
  for (auto& dir : dirs)
    for (auto& img : list_images(dir)) {
      if (file_size(img) > (2ull << 30)) continue;
      n++;
      compare_mount(img, false, &t_in, &t_rpc, &nent);
      // 未再生のジャーナルなどで拒否されたら、確認済みとして開き直したときも同じになること
      Opened probe = open_inproc(img, false, false);
      if (!probe.v && (probe.err == EUCLEAN || probe.err == 117)) compare_mount(img, true, &t_in, &t_rpc, &nent);
    }
  if (n == 0) { std::fprintf(stderr, "no images found\n"); return 77; }
  std::fprintf(stderr, "read: %d images, %d mounts compared, entries=%llu, tree walk total: inproc=%.1fms rpc=%.1fms (x%.2f)\n", n, g_checked,
               (unsigned long long)nent, t_in, t_rpc, t_in > 0 ? t_rpc / t_in : 0.0);
  return g_fail ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// write
std::vector<uint8_t> pattern(uint32_t seed, size_t n) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; i++) v[i] = static_cast<uint8_t>((i * 131 + seed * 7 + (i >> 9)) & 0xFF);
  return v;
}

// 操作列。各操作の戻り値を log に残す (FS が未対応で失敗する操作 (FAT の symlink など) は、失敗の errno が同じことを確かめる)。
void script(FsVolume& v, std::vector<std::string>* log) {
  auto L = [&](const std::string& what, int64_t rc) { log->push_back(what + "=" + std::to_string(rc)); };
  const uint64_t root = v.root();
  uint64_t d = 0, sub = 0, small = 0, multi = 0, big = 0, tmp = 0, lng = 0, jp = 0, lnk = 0;
  // ---- CreateOnly の範囲 (作る・書く・sync)
  L("mkdir kfsd", v.mkdir(root, "kfsd", 0755, &d));
  L("mkdir sub", v.mkdir(d, "sub", 0755, &sub));
  L("create small", v.create(d, "small.txt", 0644, &small));
  auto p100 = pattern(1, 100);
  L("write small", v.write(small, 0, p100.data(), p100.size()));
  L("create multi", v.create(d, "multi.bin", 0644, &multi));
  auto pm = pattern(2, 3 << 20);
  const size_t pieces[] = {4096, 65536 + 13, 1 << 20, 777};
  size_t off = 0, pi = 0;
  while (off < pm.size()) {
    size_t n = std::min(pieces[pi++ % 4], pm.size() - off);
    int64_t r = v.write(multi, off, pm.data() + off, n);
    if (r != static_cast<int64_t>(n)) { L("write multi piece", r); break; }
    off += n;
  }
  L("multi written", static_cast<int64_t>(off));
  L("create big", v.create(d, "big.bin", 0644, &big));
  auto pb = pattern(3, 5 << 20);   // 4 MiB (kMaxIo) を超える 1 回の write
  L("write big", v.write(big, 0, pb.data(), pb.size()));
  L("create long name", v.create(d, std::string(200, 'L') + ".txt", 0644, &lng));
  L("create jp name", v.create(d, "日本語 ファイル.txt", 0644, &jp));
  if (jp) L("write jp", v.write(jp, 0, "こんにちは", 15));
  L("sync 1", v.sync());
  // ---- Full の範囲 (変更・縮める・名前の変更・削除・リンク)
  L("truncate multi", v.truncate(multi, 1 << 20));
  auto pp = pattern(4, 5000);
  L("write_inplace multi", v.write_inplace(multi, 1000, pp.data(), pp.size()));
  L("write multi extend", v.write(multi, (1 << 20) + 4096, pp.data(), pp.size()));
  L("write_inplace beyond", v.write_inplace(small, 1000, pp.data(), 10));
  L("rename small", v.rename(d, "small.txt", sub, "renamed.txt"));
  L("symlink", v.symlink(d, "lnk", "multi.bin", &lnk));
  uint64_t t2 = 0;
  L("link", v.link(d, "hard", multi));
  L("create tmp a", v.create(sub, "a", 0644, &tmp));
  L("create tmp b", v.create(sub, "b", 0644, &tmp));
  L("unlink a", v.unlink(sub, "a"));
  L("rmdir nonempty", v.rmdir(d, "sub"));
  L("unlink hard", v.unlink(d, "hard"));
  L("unlink b", v.unlink(sub, "b"));
  L("unlink renamed", v.unlink(sub, "renamed.txt"));
  L("rmdir sub", v.rmdir(d, "sub"));
  // ---- エラー
  L("create exists", v.create(d, "multi.bin", 0644, &t2));
  L("mkdir exists", v.mkdir(d, "multi.bin", 0755, &t2));
  L("lookup missing", v.lookup(d, "missing", &t2));
  L("unlink missing", v.unlink(d, "missing"));
  L("rmdir file", v.rmdir(d, "multi.bin"));
  L("rename missing", v.rename(d, "missing", d, "x"));
  L("unlink dir", v.unlink(root, "kfsd"));
  L("rmdir non-empty root dir", v.rmdir(root, "kfsd"));
  // ---- 結果の確認
  FsStat st;
  L("stat multi", v.stat(multi, &st));
  L("size multi", static_cast<int64_t>(st.size));
  std::vector<uint8_t> back(st.size);
  L("read multi", v.read(multi, 0, back.data(), back.size()));
  auto h = Sha256::hash(back.data(), back.size());
  char hx[17];
  std::snprintf(hx, sizeof hx, "%02x%02x%02x%02x%02x%02x%02x%02x", h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]);
  log->push_back(std::string("sha multi=") + hx);
  L("sync 2", v.sync());
}

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
    for (ssize_t i = 0; i < n && zero; i += 8) { uint64_t w; std::memcpy(&w, &buf[static_cast<size_t>(i)], std::min<size_t>(8, static_cast<size_t>(n - i))); if (w) zero = false; }
    if (!zero && ::pwrite(out, buf.data(), static_cast<size_t>(n), static_cast<off_t>(off)) != n) ok = false;
  }
  ::close(in);
  ::close(out);
  return ok;
}

std::string make_workdir() {
  std::string base = ::access("/dev/shm", W_OK) == 0 ? "/dev/shm" : "/tmp";
  std::string t = base + "/usbmem_kfsd_wXXXXXX";
  std::vector<char> b(t.begin(), t.end());
  b.push_back('\0');
  return ::mkdtemp(b.data()) ? std::string(b.data()) : std::string();
}

int run_write(const std::vector<std::string>& dirs) {
  std::string work = make_workdir();
  if (work.empty()) { std::fprintf(stderr, "cannot create workdir\n"); return 1; }
  int done = 0, skipped_ro = 0;
  bool no_write_build = false;
  for (auto& dir : dirs) {
    int per_dir = 0;
    for (auto& img : list_images(dir)) {
      if (per_dir >= 3) break;
      if (file_size(img) > (256ull << 20)) continue;
      std::string A = work + "/A.img", B = work + "/B.img";
      if (!copy_sparse(img, A) || !copy_sparse(img, B)) { std::fprintf(stderr, "copy failed\n"); g_fail++; continue; }
      std::vector<std::string> la, lb;
      std::string fs;
      {
        Opened a = open_inproc(A, true);
        if (!a.v) {
          if (a.err == EROFS) no_write_build = true;
          continue;
        }
        if (!a.v->writable()) { skipped_ro++; continue; }
        fs = a.fs_type;
        double t0 = now_ms();
        script(*a.v, &la);
        a.v.reset();
        std::fprintf(stderr, "  %-60s %-8s inproc script %.1fms\n", img.c_str(), fs.c_str(), now_ms() - t0);
      }
      {
        Opened b = open_rpc(B, true);
        CHECK(b.v != nullptr);
        if (!b.v) { std::fprintf(stderr, "  rpc mount failed: err=%d\n", b.err); continue; }
        CHECK(b.v->writable());
        double t0 = now_ms();
        script(*b.v, &lb);
        uint64_t rc, rb, wc, wb;
        b.rpc->dev_stats(&rc, &rb, &wc, &wb);
        b.v.reset();   // kClose = 正常なアンマウント (NTFS のダーティ解除・ext4 のジャーナル終了)
        std::fprintf(stderr, "  %-60s %-8s rpc script    %.1fms (dev reads=%llu writes=%llu)\n", img.c_str(), fs.c_str(), now_ms() - t0, (unsigned long long)rc, (unsigned long long)wc);
      }
      if (!same(la, lb, img + " op results")) g_fail++;
      // 書いた結果のツリー (mtime は書いた時刻なので比べない)
      Opened ra = open_inproc(A, false), rb2 = open_inproc(B, false);
      CHECK(ra.v && rb2.v);
      if (ra.v && rb2.v) {
        std::vector<std::string> ta, tb;
        DumpStats sa, sb;
        dump(*ra.v, ra.v->root(), "", false, &ta, &sa);
        dump(*rb2.v, rb2.v->root(), "", false, &tb, &sb);
        if (!same(ta, tb, img + " tree after writes")) g_fail++;
        CHECK_EQ(ra.v->state(), rb2.v->state());
        CHECK(sa.entries > 3);
      }
      ra.v.reset(); rb2.v.reset();
      ::unlink(A.c_str());
      ::unlink(B.c_str());
      per_dir++;
      done++;
      g_checked++;
    }
  }
  ::rmdir(work.c_str());
  if (no_write_build && done == 0) { std::fprintf(stderr, "write support is not built (USBMEM_NTFS_WRITE=OFF)\n"); return 77; }
  if (done == 0) { std::fprintf(stderr, "no writable images (read-only %d)\n", skipped_ro); return 77; }
  std::fprintf(stderr, "write: %d images compared\n", done);
  return g_fail ? 1 : 0;
}

// ---------------------------------------------------------------------------------------------------------------------------------
// abnormal
class SwitchDevice final : public BlockDevice {
 public:
  explicit SwitchDevice(BlockDevicePtr b) : b_(std::move(b)) {}
  Status read_at(uint64_t o, void* p, size_t n, const IoOptions& io = {}) override { reads++; return gone ? Status::Make(Errc::Disconnected, ENODEV) : b_->read_at(o, p, n, io); }
  Status write_at(uint64_t o, const void* p, size_t n, const IoOptions& io = {}) override { return gone ? Status::Make(Errc::Disconnected, ENODEV) : b_->write_at(o, p, n, io); }
  Status flush() override { return Status::Ok(); }
  Status close() override { return Status::Ok(); }
  uint64_t size() const override { return b_->size(); }
  uint32_t logical_block_size() const override { return b_->logical_block_size(); }
  bool is_read_only() const override { return b_->is_read_only(); }
  std::string describe() const override { return "switch"; }
  std::atomic<bool> gone{false};
  std::atomic<long> reads{0};
 private:
  BlockDevicePtr b_;
};

bool pid_gone(pid_t pid, int ms) {
  for (int i = 0; i < ms / 20; i++) {
    if (::kill(pid, 0) != 0 && errno == ESRCH) return true;
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  return ::kill(pid, 0) != 0 && errno == ESRCH;
}

int run_abnormal(const std::string& dir) {
  std::string img;
  for (auto& i : list_images(dir)) {
    Opened o = open_inproc(i, false);
    if (o.v && o.fs_type != "" && file_size(i) < (512ull << 20)) { img = i; break; }
  }
  if (img.empty()) { std::fprintf(stderr, "no mountable image in %s\n", dir.c_str()); return 77; }
  std::fprintf(stderr, "abnormal: using %s\n", img.c_str());

  // 1. kfsd を SIGKILL: 本体は落ちず、以降は -EPIPE。exit_status は SIGKILL
  {
    Opened o = open_rpc(img, false);
    CHECK(o.v != nullptr);
    if (!o.v) return 1;
    std::vector<FsDirEntry> ents;
    CHECK_EQ(o.v->readdir(o.v->root(), &ents), 0);
    pid_t pid = o.rpc->pid();
    CHECK(pid > 0);
    ::kill(pid, SIGKILL);
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    FsStat st;
    CHECK_EQ(o.v->stat(o.v->root(), &st), -EPIPE);
    CHECK_EQ(o.v->readdir(o.v->root(), &ents), -EPIPE);
    CHECK(!o.rpc->alive());
    kfsd::RpcVolume* r = o.rpc;
    (void)r;
    o.v.reset();
    CHECK(pid_gone(pid, 2000));
  }
  // 2. 本体が (kClose なしに) ソケットを閉じた: kfsd は EOF で終わる (終了コード 1)
  {
    int fd = -1;
    pid_t pid = -1;
    CHECK_EQ(kfsd::spawn(g_kfsd, {}, &fd, &pid), 0);
    ::close(fd);
    int st = 0;
    bool exited = false;
    for (int i = 0; i < 250; i++) {
      if (::waitpid(pid, &st, WNOHANG) == pid) { exited = true; break; }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    CHECK(exited);
    if (exited) { CHECK(WIFEXITED(st)); CHECK_EQ(WEXITSTATUS(st), 1); }
    else { ::kill(pid, SIGKILL); ::waitpid(pid, &st, 0); }
  }
  // 3. 本体のプロセスが死んだ (デストラクタなし): kfsd も終わる
  {
    int p[2];
    CHECK_EQ(::pipe(p), 0);
    pid_t child = ::fork();
    if (child == 0) {
      ::close(p[0]);
      Opened o = open_rpc(img, false);
      pid_t kp = o.v ? o.rpc->pid() : -1;
      ::write(p[1], &kp, sizeof kp);
      ::_exit(0);   // o のデストラクタを走らせない (kClose なし)
    }
    ::close(p[1]);
    pid_t kp = -1;
    CHECK_EQ(::read(p[0], &kp, sizeof kp), (ssize_t)sizeof kp);
    ::close(p[0]);
    int st;
    ::waitpid(child, &st, 0);
    CHECK(kp > 0);
    if (kp > 0) {
      bool gone = pid_gone(kp, 5000);
      CHECK(gone);
      if (!gone) ::kill(kp, SIGKILL);
    }
  }
  // 4. 応答しない kfsd (SIGSTOP): タイムアウトで見切り、kfsd は殺される
  {
    auto dev = open_dev(img, false);
    kfsd::MountOptions mo;
    mo.timeout_ms = 300;
    int err = 0;
    auto r = kfsd::RpcVolume::mount(g_kfsd, dev, mo, &err);
    CHECK(r != nullptr);
    if (r) {
      pid_t pid = r->pid();
      ::kill(pid, SIGSTOP);
      FsStat st;
      double t0 = now_ms();
      int rc = r->stat(r->root(), &st);
      double ms = now_ms() - t0;
      CHECK_EQ(rc, -ETIMEDOUT);
      CHECK(ms >= 250 && ms < 3000);
      CHECK(!r->alive());
      r.reset();
      CHECK(pid_gone(pid, 2000));
    }
  }
  // 5. デバイスの抜去: 読めなくなったら負の errno。本体も kfsd も落ちない
  {
    auto sw = std::make_shared<SwitchDevice>(open_dev(img, false));
    kfsd::MountOptions mo;
    int err = 0;
    auto r = kfsd::RpcVolume::mount(g_kfsd, sw, mo, &err);
    CHECK(r != nullptr);
    if (r) {
      std::vector<FsDirEntry> ents;
      CHECK_EQ(r->readdir(r->root(), &ents), 0);
      // 未読の通常ファイル (サイズ > 0) を 1 つ探す (まだページキャッシュにない = デバイスを読みに行く)
      std::vector<std::string> lines;
      DumpStats ds;
      uint64_t target = 0;
      std::vector<std::pair<uint64_t, uint64_t>> files;   // (ino, size) を 1 階層だけ探す
      for (auto& e : ents) {
        FsStat st;
        if (r->stat(e.ino, &st) == 0 && (st.mode & 0170000) == 0100000 && st.size >= 4096) { target = e.ino; break; }
      }
      sw->gone = true;
      if (target) {
        std::vector<uint8_t> buf(65536);
        int64_t rc = r->read(target, 0, buf.data(), buf.size());
        CHECK(rc < 0);
        std::fprintf(stderr, "  read after device gone: rc=%lld\n", (long long)rc);
      } else {
        std::fprintf(stderr, "  (no unread regular file at top level; device-gone read check skipped)\n");
      }
      CHECK(r->alive());   // デバイスの失敗は kfsd の死ではない
      sw->gone = false;
    }
  }
  // 6. マウントに失敗するデバイス (ゼロ): プロセス内と同じ errno。kfsd は終了コード 3
  {
    std::string zero = make_workdir() + "/zero.img";
    {
      int fd = ::open(zero.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
      CHECK(fd >= 0);
      if (fd >= 0) { CHECK_EQ(::ftruncate(fd, 4 << 20), 0); ::close(fd); }
    }
    Opened a = open_inproc(zero, false), b = open_rpc(zero, false);
    CHECK(a.v == nullptr && b.v == nullptr);
    CHECK_EQ(a.err, b.err);
    std::fprintf(stderr, "  garbage device: inproc err=%d rpc err=%d\n", a.err, b.err);
    ::unlink(zero.c_str());
    ::rmdir(zero.substr(0, zero.rfind('/')).c_str());
  }
  // 7. kfsd が無い
  {
    auto dev = open_dev(img, false);
    int err = 0;
    CHECK(kfsd::RpcVolume::mount("/nonexistent/kfsd", dev, {}, &err) == nullptr);
    CHECK_EQ(err, kfsd::kErrNoKfsd);
  }
  std::fprintf(stderr, "abnormal: %s\n", g_fail ? "FAILED" : "ok");
  return g_fail ? 1 : 0;
}

}  // namespace

int main(int argc, char** argv) {
  std::vector<std::string> args(argv + 1, argv + argc);
  std::string mode;
  std::vector<std::string> rest;
  for (size_t i = 0; i < args.size(); i++) {
    if (args[i] == "--kfsd" && i + 1 < args.size()) g_kfsd = args[++i];
    else if (mode.empty()) mode = args[i];
    else rest.push_back(args[i]);
  }
  if (g_kfsd.empty()) g_kfsd = kfsd::find_exe();
  if (g_kfsd.empty() || mode.empty() || rest.empty()) {
    std::fprintf(stderr, "usage: %s --kfsd <kfsd> read|write|abnormal <fixture dir>...\n", argv[0]);
    return 64;
  }
  if (mode == "read") return run_read(rest);
  if (mode == "write") return run_write(rest);
  if (mode == "abnormal") return run_abnormal(rest[0]);
  std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
  return 64;
}
