// SPDX-License-Identifier: Apache-2.0
// kfsd のプロトコル・クライアント (RpcVolume)・サーバの共通部分 (serve) の試験。ドライバ (カーネル由来のコード) は使わない:
// サーバ側はメモリ上の FsVolume (MemFs) を返す MountFn。公開版でも動く。
//   1 本のソケットの両端を 1 つのプロセスのスレッドで持ち、(a) 全操作が MemFs を直接呼んだ結果と一致すること、(b) 大きな read / write の分割、
//   (c) readdir のページング、(d) デバイスの callback (読み・書き・フラッシュ・エラー)、(e) 通信が壊れたとき (EOF / タイムアウト / 不正な応答)
//   に、クライアントが落ちずに負の errno を返すこと、を確かめる。
#include <sys/socket.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "kfsd_io.h"
#include "usbmem/devices.h"
#include "usbmem/kfsd_client.h"
#include "usbmem/kfsd_server.h"
#include "usbmem/sha256.h"

using namespace usbmem;

static int g_fail = 0;
#define CHECK(c) do { if (!(c)) { std::fprintf(stderr, "CHECK failed %s:%d: %s\n", __FILE__, __LINE__, #c); g_fail++; } } while (0)
#define CHECK_EQ(a, b) do { long long va_ = (long long)(a), vb_ = (long long)(b); if (va_ != vb_) { std::fprintf(stderr, "CHECK_EQ failed %s:%d: %s (%lld) != %s (%lld)\n", __FILE__, __LINE__, #a, va_, #b, vb_); g_fail++; } } while (0)

namespace {

constexpr uint32_t S_DIR = 040000, S_REG = 0100000, S_LNK = 0120000;

// メモリ上の FsVolume。ドライバの代わりに、RPC の両端を試すための参照実装 (ここの結果が「プロセス内」の基準になる)。
class MemFs final : public FsVolume {
 public:
  struct Node { uint32_t mode = 0, nlink = 1; int64_t mtime = 0; std::string data; std::map<std::string, uint64_t> kids; };
  explicit MemFs(bool rw = true) : rw_(rw) { nodes_[1].mode = S_DIR | 0755; nodes_[1].nlink = 2; }
  const char* fs_name() const override { return "memfs"; }
  uint64_t root() const override { return 1; }
  bool writable() const override { return rw_; }
  uint32_t state() const override { return 0x10; }
  int lookup(uint64_t dir, const std::string& n, uint64_t* ino) override {
    auto d = nodes_.find(dir);
    if (d == nodes_.end() || (d->second.mode & 0170000) != S_DIR) return -ENOTDIR;
    auto k = d->second.kids.find(n);
    if (k == d->second.kids.end()) return -ENOENT;
    *ino = k->second;
    return 0;
  }
  int stat(uint64_t ino, FsStat* st) override {
    auto it = nodes_.find(ino);
    if (it == nodes_.end()) return -ENOENT;
    st->ino = ino; st->size = it->second.data.size(); st->mode = it->second.mode; st->nlink = it->second.nlink; st->mtime = it->second.mtime;
    return 0;
  }
  int readdir(uint64_t dir, std::vector<FsDirEntry>* out) override {
    auto d = nodes_.find(dir);
    if (d == nodes_.end() || (d->second.mode & 0170000) != S_DIR) return -ENOTDIR;
    out->clear();
    for (auto& k : d->second.kids) out->push_back({k.first, k.second, ((nodes_[k.second].mode & 0170000) == S_DIR) ? 4u : ((nodes_[k.second].mode & 0170000) == S_LNK ? 10u : 8u)});
    return 0;
  }
  int64_t read(uint64_t ino, uint64_t off, void* buf, size_t len) override {
    auto it = nodes_.find(ino);
    if (it == nodes_.end()) return -ENOENT;
    if ((it->second.mode & 0170000) == S_DIR) return -EISDIR;
    if (hook_read) hook_read();
    if (off >= it->second.data.size()) return 0;
    size_t n = std::min<size_t>(len, it->second.data.size() - off);
    std::memcpy(buf, it->second.data.data() + off, n);
    return static_cast<int64_t>(n);
  }
  int readlink(uint64_t ino, std::string* t) override {
    auto it = nodes_.find(ino);
    if (it == nodes_.end()) return -ENOENT;
    if ((it->second.mode & 0170000) != S_LNK) return -EINVAL;
    *t = it->second.data;
    return 0;
  }
  int sync() override { return rw_ ? (sync_hook ? sync_hook() : 0) : -EROFS; }
  int add(uint64_t dir, const std::string& n, uint32_t mode, const std::string& data, uint64_t* ino) {
    if (!rw_) return -EROFS;
    auto d = nodes_.find(dir);
    if (d == nodes_.end() || (d->second.mode & 0170000) != S_DIR) return -ENOTDIR;
    if (d->second.kids.count(n)) return -EEXIST;
    uint64_t i = ++next_;
    Node& x = nodes_[i];
    x.mode = mode; x.data = data; x.mtime = 1000 + static_cast<int64_t>(i);
    if ((mode & 0170000) == S_DIR) x.nlink = 2;
    nodes_[dir].kids[n] = i;
    if (ino) *ino = i;
    return 0;
  }
  int create(uint64_t dir, const std::string& n, uint32_t mode, uint64_t* ino) override { return add(dir, n, S_REG | (mode & 07777), "", ino); }
  int mkdir(uint64_t dir, const std::string& n, uint32_t mode, uint64_t* ino) override { return add(dir, n, S_DIR | (mode & 07777), "", ino); }
  int symlink(uint64_t dir, const std::string& n, const std::string& t, uint64_t* ino) override { return add(dir, n, S_LNK | 0777, t, ino); }
  int link(uint64_t dir, const std::string& n, uint64_t target) override {
    if (!rw_) return -EROFS;
    auto t = nodes_.find(target);
    if (t == nodes_.end()) return -ENOENT;
    if ((t->second.mode & 0170000) == S_DIR) return -EPERM;
    if (nodes_[dir].kids.count(n)) return -EEXIST;
    nodes_[dir].kids[n] = target;
    t->second.nlink++;
    return 0;
  }
  int unlink(uint64_t dir, const std::string& n) override {
    if (!rw_) return -EROFS;
    uint64_t i;
    int rc = lookup(dir, n, &i);
    if (rc) return rc;
    if ((nodes_[i].mode & 0170000) == S_DIR) return -EISDIR;
    nodes_[dir].kids.erase(n);
    if (--nodes_[i].nlink == 0) nodes_.erase(i);
    return 0;
  }
  int rmdir(uint64_t dir, const std::string& n) override {
    if (!rw_) return -EROFS;
    uint64_t i;
    int rc = lookup(dir, n, &i);
    if (rc) return rc;
    if ((nodes_[i].mode & 0170000) != S_DIR) return -ENOTDIR;
    if (!nodes_[i].kids.empty()) return -ENOTEMPTY;
    nodes_[dir].kids.erase(n);
    nodes_.erase(i);
    return 0;
  }
  int rename(uint64_t od, const std::string& on, uint64_t nd, const std::string& nn) override {
    if (!rw_) return -EROFS;
    uint64_t i;
    int rc = lookup(od, on, &i);
    if (rc) return rc;
    uint64_t j;
    if (lookup(nd, nn, &j) == 0) return -EEXIST;
    nodes_[od].kids.erase(on);
    nodes_[nd].kids[nn] = i;
    return 0;
  }
  int64_t write(uint64_t ino, uint64_t off, const void* buf, size_t len) override {
    if (!rw_) return -EROFS;
    auto it = nodes_.find(ino);
    if (it == nodes_.end()) return -ENOENT;
    if (it->second.data.size() < off + len) it->second.data.resize(off + len, '\0');
    std::memcpy(&it->second.data[off], buf, len);
    return static_cast<int64_t>(len);
  }
  int64_t write_inplace(uint64_t ino, uint64_t off, const void* buf, size_t len) override {
    auto it = nodes_.find(ino);
    if (it == nodes_.end()) return -ENOENT;
    if (off + len > it->second.data.size()) return -EINVAL;   // 拡張しない
    return write(ino, off, buf, len);
  }
  int truncate(uint64_t ino, uint64_t size) override {
    if (!rw_) return -EROFS;
    auto it = nodes_.find(ino);
    if (it == nodes_.end()) return -ENOENT;
    it->second.data.resize(size, '\0');
    return 0;
  }
  std::function<void()> hook_read;
  std::function<int()> sync_hook;
  std::map<uint64_t, Node> nodes_;
 private:
  bool rw_;
  uint64_t next_ = 1;
};

// 出力用: ツリー全体を「path|type|size|mode|nlink|mtime|sha」の行にする (ino は含めない)。
static void dump(FsVolume& v, uint64_t dir, const std::string& path, std::vector<std::string>* out, int depth = 0) {
  std::vector<FsDirEntry> ents;
  int rc = v.readdir(dir, &ents);
  if (rc) { out->push_back(path + "|readdir=" + std::to_string(rc)); return; }
  std::sort(ents.begin(), ents.end(), [](const FsDirEntry& a, const FsDirEntry& b) { return a.name < b.name; });
  for (auto& e : ents) {
    FsStat st;
    int sr = v.stat(e.ino, &st);
    std::string line = path + "/" + e.name + "|" + std::to_string(e.dtype) + "|" + std::to_string(sr);
    if (sr == 0) {
      line += "|" + std::to_string(st.size) + "|" + std::to_string(st.mode) + "|" + std::to_string(st.nlink) + "|" + std::to_string(st.mtime);
      if ((st.mode & 0170000) == S_REG) {
        std::vector<uint8_t> b(st.size);
        int64_t r = st.size ? v.read(e.ino, 0, b.data(), b.size()) : 0;
        line += "|r=" + std::to_string(r);
        auto h = Sha256::hash(b.data(), b.size());
        char hx[9];
        std::snprintf(hx, sizeof hx, "%02x%02x%02x%02x", h[0], h[1], h[2], h[3]);
        line += hx;
      } else if ((st.mode & 0170000) == S_LNK) {
        std::string t;
        v.readlink(e.ino, &t);
        line += "|->" + t;
      }
    }
    out->push_back(line);
    if (sr == 0 && (st.mode & 0170000) == S_DIR && depth < 8) dump(v, e.ino, path + "/" + e.name, out, depth + 1);
  }
}

// ソケットの両端を持つ「サーバのスレッド」。serve() が返したコードを code に残す。
struct Harness {
  int cli = -1, srv = -1;
  std::thread th;
  std::atomic<int> code{-1};
  ~Harness() {
    if (cli >= 0) ::close(cli);
    if (th.joinable()) th.join();
    if (srv >= 0) ::close(srv);
  }
  // mf: サーバの MountFn
  void start(kfsd::MountFn mf) {
    int sv[2];
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);
    cli = sv[0]; srv = sv[1];
    th = std::thread([this, mf] { code = kfsd::serve(srv, mf); });
  }
};

static std::vector<uint8_t> pattern(uint32_t seed, size_t n) {
  std::vector<uint8_t> v(n);
  for (size_t i = 0; i < n; i++) v[i] = static_cast<uint8_t>((i * 31 + seed * 7 + (i >> 8)) & 0xFF);
  return v;
}

static kfsd::MountFn memfs_mount(MemFs** out_fs, bool rw = true, bool read_head = true) {
  return [out_fs, rw, read_head](BlockDevicePtr dev, const kfsd::MountParams&) {
    kfsd::MountResult r;
    auto fs = std::make_unique<MemFs>(rw);
    if (read_head) {   // デバイスの先頭 4 KiB を /devhead として見せる (callback の試験)
      std::vector<uint8_t> b(4096);
      Status st = dev->read_at(0, b.data(), b.size());
      if (!st.ok()) { r.err = st.sys_errno ? st.sys_errno : EIO; return r; }
      uint64_t ino;
      fs->add(1, "devhead", S_REG | 0644, std::string(b.begin(), b.end()), &ino);
    }
    MemFs* raw = fs.get();
    BlockDevicePtr d = dev;
    fs->sync_hook = [d]() {   // sync でデバイスへ印を書いて flush する (Dev* の書き込み経路の試験)
      if (d->is_read_only()) return 0;
      const char m[8] = {'S', 'Y', 'N', 'C', 'E', 'D', '!', '!'};
      Status st = d->write_at(8, m, sizeof m);
      if (!st.ok()) return -(st.sys_errno ? st.sys_errno : EIO);
      st = d->flush();
      return st.ok() ? 0 : -EIO;
    };
    if (out_fs) *out_fs = raw;
    r.vol = std::move(fs);
    r.fs_type = "memfs";
    return r;
  };
}

}  // namespace

// ---------------------------------------------------------------------------------------------------------------------------------
// 全操作が、MemFs を直接呼んだ結果と一致する (RPC 越しと直接で、同じ操作列から同じツリー・同じ戻り値)
static void test_ops_equivalent() {
  std::fprintf(stderr, "ops_equivalent\n");
  for (int prefetch = 0; prefetch < 2; prefetch++) {
    Harness h;
    MemFs* served = nullptr;
    h.start(memfs_mount(&served));
    auto dev = std::make_shared<MemoryDevice>(pattern(1, 1 << 16));
    kfsd::MountOptions mo;
    mo.rw = true;
    mo.prefetch_stat = prefetch != 0;
    int err = -1;
    auto rv = kfsd::RpcVolume::mount_fd(h.cli, -1, dev, mo, &err);
    h.cli = -1;   // 所有は RpcVolume へ移った
    CHECK(rv != nullptr);
    if (!rv) return;
    CHECK(std::string(rv->fs_name()) == "memfs");
    CHECK_EQ(rv->root(), 1);
    CHECK(rv->fs_type() == "memfs");
    CHECK(rv->writable());
    CHECK_EQ(rv->state(), 0x10);

    MemFs ref(true);   // 直接呼ぶ側 (同じ操作列を流す)
    {
      uint64_t unused;
      std::vector<uint8_t> b(4096);
      dev->read_at(0, b.data(), b.size());
      ref.add(1, "devhead", S_REG | 0644, std::string(b.begin(), b.end()), &unused);
    }
    auto both = [&](const char* what, const std::function<int(FsVolume&)>& f) {
      int a = f(*rv), b = f(ref);
      if (a != b) { std::fprintf(stderr, "  %s: rpc=%d direct=%d\n", what, a, b); g_fail++; }
    };
    uint64_t ia = 0, ib = 0;
    auto mk = [&](const char* what, std::function<int(FsVolume&, uint64_t*)> f) {
      int a = f(*rv, &ia), b = f(ref, &ib);
      if (a != b) { std::fprintf(stderr, "  %s: rpc=%d direct=%d\n", what, a, b); g_fail++; }
      return std::make_pair(ia, ib);
    };
    auto d = mk("mkdir d", [](FsVolume& v, uint64_t* i) { return v.mkdir(1, "d", 0755, i); });
    auto f1 = mk("create f1", [&](FsVolume& v, uint64_t* i) { return v.create(v.root(), "f1", 0644, i); });
    auto data = pattern(3, 100000);
    both("write f1", [&](FsVolume& v) { uint64_t i; v.lookup(1, "f1", &i); return static_cast<int>(v.write(i, 0, data.data(), data.size())); });
    both("write_inplace", [&](FsVolume& v) { uint64_t i; v.lookup(1, "f1", &i); return static_cast<int>(v.write_inplace(i, 10, "HELLO", 5)); });
    both("write_inplace beyond", [&](FsVolume& v) { uint64_t i; v.lookup(1, "f1", &i); return static_cast<int>(v.write_inplace(i, 200000, "X", 1)); });
    both("truncate", [&](FsVolume& v) { uint64_t i; v.lookup(1, "f1", &i); return v.truncate(i, 90000); });
    mk("symlink", [&](FsVolume& v, uint64_t* i) { return v.symlink(1, "ln", "f1", i); });
    both("link", [&](FsVolume& v) { uint64_t i; v.lookup(1, "f1", &i); return v.link(1, "f1hard", i); });
    both("rename", [&](FsVolume& v) { uint64_t di; v.lookup(1, "d", &di); return v.rename(1, "f1hard", di, "moved"); });
    both("rename missing", [&](FsVolume& v) { return v.rename(1, "nonexistent", 1, "x"); });
    both("unlink dir", [&](FsVolume& v) { return v.unlink(1, "d"); });
    both("rmdir nonempty", [&](FsVolume& v) { return v.rmdir(1, "d"); });
    both("lookup missing", [&](FsVolume& v) { uint64_t i; return v.lookup(1, "nope", &i); });
    both("lookup in file", [&](FsVolume& v) { uint64_t i, f; v.lookup(1, "f1", &f); return v.lookup(f, "x", &i); });
    both("mkdir exists", [&](FsVolume& v) { uint64_t i; return v.mkdir(1, "d", 0755, &i); });
    both("sync", [&](FsVolume& v) { return v.sync(); });
    CHECK_EQ(rv->lookup(1, "a/b", &ia), -EINVAL);   // 名前に '/' は入れない (kfsd へ送る前に弾く)
    CHECK_EQ(rv->lookup(1, "", &ia), -EINVAL);
    std::vector<std::string> x, y;
    dump(*rv, 1, "", &x);
    dump(ref, 1, "", &y);
    CHECK(x == y);
    CHECK(x.size() >= 5);
    if (x != y) for (size_t i = 0; i < std::max(x.size(), y.size()); i++) std::fprintf(stderr, "  [%zu] rpc=%s | direct=%s\n", i, i < x.size() ? x[i].c_str() : "-", i < y.size() ? y[i].c_str() : "-");
    // 範囲外・短い読み
    uint64_t f;
    CHECK_EQ(rv->lookup(1, "f1", &f), 0);
    uint8_t tmp[16];
    CHECK_EQ(rv->read(f, 89990, tmp, 16), 10);   // 末尾で短くなる (EOF)
    CHECK_EQ(rv->read(f, 90000, tmp, 16), 0);
    CHECK_EQ(rv->read(f, 0, tmp, 0), 0);
    // デバイスの callback: devhead の中身 = デバイスの先頭、sync でデバイスへ印が書かれる
    uint64_t dh;
    CHECK_EQ(rv->lookup(1, "devhead", &dh), 0);
    std::vector<uint8_t> hd(4096);
    CHECK_EQ(rv->read(dh, 0, hd.data(), hd.size()), 4096);
    CHECK(std::memcmp(hd.data(), pattern(1, 1 << 16).data(), 4096) == 0);
    CHECK_EQ(rv->sync(), 0);
    CHECK(std::memcmp(dev->bytes().data() + 8, "SYNCED!!", 8) == 0);
    uint64_t rc, rb, wc, wb;
    rv->dev_stats(&rc, &rb, &wc, &wb);
    CHECK(rc >= 1 && rb >= 4096);
    CHECK(wc >= 1 && wb >= 8);
    kfsd::StatsRep sr;
    CHECK_EQ(rv->remote_stats(&sr), 0);
    CHECK(sr.dev_read_calls == rc && sr.requests > 20);
    if (prefetch) CHECK(rv->stat_cache_hits() > 0);
    rv.reset();   // kClose → サーバは 0 で終わる
    h.th.join();
    CHECK_EQ(h.code.load(), 0);
  }
}

// 4 MiB (kMaxIo) を超える read / write は分割して送られ、内容が壊れない
static void test_large_io() {
  std::fprintf(stderr, "large_io\n");
  Harness h;
  MemFs* served = nullptr;
  h.start(memfs_mount(&served, true, false));
  auto dev = std::make_shared<MemoryDevice>(pattern(1, 1 << 16));
  kfsd::MountOptions mo;
  mo.rw = true;
  int err;
  auto rv = kfsd::RpcVolume::mount_fd(h.cli, -1, dev, mo, &err);
  h.cli = -1;
  CHECK(rv != nullptr);
  if (!rv) return;
  uint64_t f;
  CHECK_EQ(rv->create(1, "big", 0644, &f), 0);
  auto data = pattern(9, 10 * 1024 * 1024 + 123);
  CHECK_EQ(rv->write(f, 0, data.data(), data.size()), (long long)data.size());
  CHECK_EQ(rv->write_inplace(f, 5000000, data.data(), 5 * 1024 * 1024), 5 * 1024 * 1024);   // 4 MiB + 1 MiB に分かれる (ファイルの内側)
  std::vector<uint8_t> expect = data;
  std::memcpy(expect.data() + 5000000, data.data(), 5 * 1024 * 1024);
  std::vector<uint8_t> back(expect.size());
  CHECK_EQ(rv->read(f, 0, back.data(), back.size()), (long long)back.size());
  CHECK(back == expect);
  CHECK(served && served->nodes_[2].data.size() == expect.size());
  // 範囲の途中から (大きなオフセット + 4 MiB 境界またぎ)
  std::vector<uint8_t> part(9 * 1024 * 1024);
  CHECK_EQ(rv->read(f, 777, part.data(), part.size()), (long long)part.size());
  CHECK(std::memcmp(part.data(), expect.data() + 777, part.size()) == 0);
}

// readdir のページング: 大きなディレクトリを、重複・欠落なしに返す。prefetch_stat なら stat がキャッシュから返る
static void test_readdir_paging() {
  std::fprintf(stderr, "readdir_paging\n");
  for (int prefetch = 0; prefetch < 2; prefetch++) {
    Harness h;
    MemFs* served = nullptr;
    h.start(memfs_mount(&served, true, false));
    auto dev = std::make_shared<MemoryDevice>(pattern(1, 4096));
    kfsd::MountOptions mo;
    mo.rw = true;
    mo.prefetch_stat = prefetch != 0;
    int err;
    auto rv = kfsd::RpcVolume::mount_fd(h.cli, -1, dev, mo, &err);
    h.cli = -1;
    CHECK(rv != nullptr);
    if (!rv) return;
    const int N = 30000;
    for (int i = 0; i < N; i++) {
      char nm[64];
      std::snprintf(nm, sizeof nm, "file-with-a-fairly-long-name-%06d.txt", i);
      served->add(1, nm, S_REG | 0644, std::string(i % 7, 'x'), nullptr);
    }
    std::vector<FsDirEntry> ents;
    CHECK_EQ(rv->readdir(1, &ents), 0);
    CHECK_EQ(ents.size(), N);
    std::map<std::string, int> seen;
    for (auto& e : ents) seen[e.name]++;
    CHECK_EQ(seen.size(), N);
    int bad = 0;
    for (auto& e : ents) {
      FsStat st;
      if (rv->stat(e.ino, &st) != 0 || st.ino != e.ino) bad++;
    }
    CHECK_EQ(bad, 0);
    if (prefetch) CHECK(rv->stat_cache_hits() >= (uint64_t)N);
    else CHECK_EQ(rv->stat_cache_hits(), 0);
    // 変更系で stat キャッシュは捨てられる (古い値を返さない)
    FsStat st0, st1;
    CHECK_EQ(rv->stat(ents[5].ino, &st0), 0);
    CHECK_EQ(rv->truncate(ents[5].ino, 1234), 0);
    CHECK_EQ(rv->stat(ents[5].ino, &st1), 0);
    CHECK_EQ(st1.size, 1234);
  }
}

// デバイスの callback の失敗が、マウントの失敗 / 読みの失敗として errno で返る
namespace {
class FailingDevice final : public BlockDevice {
 public:
  explicit FailingDevice(int ok_reads, int e = EIO) : ok_(ok_reads), e_(e) {}
  Status read_at(uint64_t off, void* buf, size_t len, const IoOptions& = {}) override {
    if (ok_-- <= 0) return Status::Make(Errc::Io, e_);
    std::memset(buf, static_cast<int>(off & 0xFF), len);
    return Status::Ok();
  }
  Status write_at(uint64_t, const void*, size_t, const IoOptions& = {}) override { return Status::Make(Errc::ReadOnly); }
  Status flush() override { return Status::Ok(); }
  Status close() override { return Status::Ok(); }
  uint64_t size() const override { return 1 << 20; }
  uint32_t logical_block_size() const override { return 512; }
  bool is_read_only() const override { return true; }
  std::string describe() const override { return "failing"; }
 private:
  std::atomic<int> ok_; int e_;
};
}  // namespace

static void test_device_errors() {
  std::fprintf(stderr, "device_errors\n");
  {   // マウント中のデバイスの読みの失敗 → マウントが失敗し、errno が (本体のデバイスの errno で) 返る
    Harness h;
    h.start(memfs_mount(nullptr));
    auto dev = std::make_shared<FailingDevice>(0, ENODEV);
    int err = 0;
    kfsd::MountOptions mo;
    auto rv = kfsd::RpcVolume::mount_fd(h.cli, -1, dev, mo, &err);
    h.cli = -1;
    CHECK(rv == nullptr);
    CHECK_EQ(err, ENODEV);
    h.th.join();
    CHECK_EQ(h.code.load(), 3);
  }
  {   // 成功したマウントの後に、デバイスが抜かれた: read は -EIO などで失敗し、ボリュームは (kfsd は生きていて) 使い続けられる
    Harness h;
    MemFs* served = nullptr;
    h.start(memfs_mount(&served, false, true));
    auto dev = std::make_shared<FailingDevice>(1);
    int err = 0;
    kfsd::MountOptions mo;
    auto rv = kfsd::RpcVolume::mount_fd(h.cli, -1, dev, mo, &err);
    h.cli = -1;
    CHECK(rv != nullptr);
    if (!rv) return;
    CHECK(!rv->writable());
    CHECK_EQ(rv->create(1, "x", 0644, nullptr), -EROFS);
    CHECK(rv->alive());
  }
}

// kfsd が消えた (EOF) / 応答しない (タイムアウト) / 不正な応答を返す: クライアントは落ちず、負の errno を返し、以降は -EPIPE
static void test_peer_failures() {
  std::fprintf(stderr, "peer_failures\n");
  {   // EOF: サーバ側のソケットが閉じる
    Harness h;
    MemFs* served = nullptr;
    h.start(memfs_mount(&served, true, false));
    auto dev = std::make_shared<MemoryDevice>(pattern(1, 4096));
    int err;
    auto rv = kfsd::RpcVolume::mount_fd(h.cli, -1, dev, {}, &err);
    h.cli = -1;
    CHECK(rv != nullptr);
    if (!rv) return;
    uint64_t i;
    CHECK_EQ(rv->lookup(1, "x", &i), -ENOENT);
    ::shutdown(h.srv, SHUT_RDWR);
    h.th.join();
    CHECK_EQ(h.code.load(), 1);
    int rc = rv->lookup(1, "x", &i);
    CHECK_EQ(rc, -EPIPE);
    CHECK(!rv->alive());
    std::vector<FsDirEntry> e;
    CHECK_EQ(rv->readdir(1, &e), -EPIPE);
    uint8_t b[4];
    CHECK_EQ(rv->read(2, 0, b, 4), -EPIPE);
    CHECK(!rv->writable());
    CHECK_EQ(rv->state(), 0);
  }
  {   // タイムアウト: read が 2 秒止まるサーバ。クライアントは 200 ms で見切る
    Harness h;
    MemFs* served = nullptr;
    h.start(memfs_mount(&served, true, false));
    auto dev = std::make_shared<MemoryDevice>(pattern(1, 4096));
    kfsd::MountOptions mo;
    mo.timeout_ms = 200;
    int err;
    auto rv = kfsd::RpcVolume::mount_fd(h.cli, -1, dev, mo, &err);
    h.cli = -1;
    CHECK(rv != nullptr);
    if (!rv) return;
    served->add(1, "f", S_REG | 0644, "hello", nullptr);
    served->hook_read = [] { std::this_thread::sleep_for(std::chrono::milliseconds(1500)); };
    uint64_t f;
    CHECK_EQ(rv->lookup(1, "f", &f), 0);
    char b[8];
    auto t0 = std::chrono::steady_clock::now();
    CHECK_EQ(rv->read(f, 0, b, 5), -ETIMEDOUT);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    CHECK(ms >= 150 && ms < 1200);
    CHECK(!rv->alive());
    CHECK_EQ(rv->read(f, 0, b, 5), -EPIPE);
    rv.reset();   // ソケットを閉じる → サーバのスレッドは read から戻って EOF で終わる (join できる)
  }
  {   // abort(): 別スレッドから止める
    Harness h;
    MemFs* served = nullptr;
    h.start(memfs_mount(&served, true, false));
    auto dev = std::make_shared<MemoryDevice>(pattern(1, 4096));
    kfsd::MountOptions mo;
    mo.timeout_ms = 0;
    int err;
    auto rv = kfsd::RpcVolume::mount_fd(h.cli, -1, dev, mo, &err);
    h.cli = -1;
    CHECK(rv != nullptr);
    if (!rv) return;
    served->add(1, "f", S_REG | 0644, "hello", nullptr);
    served->hook_read = [] { std::this_thread::sleep_for(std::chrono::milliseconds(1000)); };
    uint64_t f;
    rv->lookup(1, "f", &f);
    std::thread t([&] { std::this_thread::sleep_for(std::chrono::milliseconds(100)); rv->abort(); });
    char b[8];
    CHECK_EQ(rv->read(f, 0, b, 5), -EPIPE);
    t.join();
    CHECK(!rv->alive());
  }
}

// 偽のサーバ (信頼できない kfsd のつもり): ハンドシェイクだけ正しく答え、次の要求にだけ不正な応答を返す
static void fake_server(int fd, int mode) {
  using namespace usbmem::kfsd;
  auto rd = [&](Hdr* h, std::vector<uint8_t>* p) {
    if (io::recv_hdr(fd, h, 0) <= 0) return false;
    p->resize(h->len);
    return h->len == 0 || io::recv_all(fd, p->data(), h->len, 0) > 0;
  };
  Hdr h;
  std::vector<uint8_t> p;
  if (!rd(&h, &p)) return;
  HelloMsg hm{};
  hm.magic = kMagic; hm.version = kVersion; hm.max_payload = kMaxPayload;
  io::send_msg(fd, Hdr{kReply, sizeof hm, h.seq, 0, 0, 0}, &hm, sizeof hm, 0);
  if (!rd(&h, &p)) return;
  MountRep mr{};
  std::strcpy(mr.info.fs_name, "evil");
  mr.info.root = 1;
  io::send_msg(fd, Hdr{kReply, sizeof mr, h.seq, 0, 0, 0}, &mr, sizeof mr, 0);
  if (!rd(&h, &p)) return;   // 次の要求 (mode ごとに不正に答える)
  std::vector<uint8_t> big;
  switch (mode) {
    case 0: io::send_msg(fd, Hdr{kReply, 0, h.seq + 7, 0, 0, 0}, nullptr, 0, 0); break;                      // seq が違う
    case 1: { Hdr bad{kReply, kMaxPayload + 1, h.seq, 0, 0, 0}; io::send_msg(fd, bad, nullptr, 0, 0); break; }   // 長すぎる payload
    case 2: {   // readdir: 名前に '/' を含むエントリ
      big.resize(sizeof(WireDirentHead) + 3);
      WireDirentHead d{5, 8, 3};
      std::memcpy(big.data(), &d, sizeof d);
      std::memcpy(big.data() + sizeof d, "a/b", 3);
      io::send_msg(fd, Hdr{kReply, static_cast<uint32_t>(big.size()), h.seq, 0, 0, 0}, big.data(), big.size(), 0);
      break;
    }
    case 3: {   // read: 要求より多く返す
      big.assign(100, 'z');
      io::send_msg(fd, Hdr{kReply, 100, h.seq, 100, 0, 0}, big.data(), 100, 0);
      break;
    }
    case 4: {   // readdir: cookie が進まない無限ページング
      big.resize(sizeof(WireDirentHead) + 1);
      WireDirentHead d{5, 8, 1};
      std::memcpy(big.data(), &d, sizeof d);
      big[sizeof d] = 'a';
      io::send_msg(fd, Hdr{kReply, static_cast<uint32_t>(big.size()), h.seq, 0, 0, 1}, big.data(), big.size(), 0);   // b = 0 (cookie 0 のまま) c = 続きあり
      break;
    }
    case 5: io::send_msg(fd, Hdr{0x999, 0, h.seq, 0, 0, 0}, nullptr, 0, 0); break;                           // 知らない op
    case 6: io::send_msg(fd, Hdr{kReply, 7, h.seq, 0, 0, 0}, "garbage", 7, 0); break;                        // stat の payload の大きさが違う
    default: break;
  }
  // 少し待ってから閉じる (クライアントが先に判断できるように)
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  ::close(fd);
}

static void test_malicious_server() {
  std::fprintf(stderr, "malicious_server\n");
  for (int mode = 0; mode <= 6; mode++) {
    int sv[2];
    CHECK(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sv) == 0);
    std::thread t(fake_server, sv[1], mode);
    auto dev = std::make_shared<MemoryDevice>(pattern(1, 4096));
    kfsd::MountOptions mo;
    mo.timeout_ms = 3000;
    int err;
    auto rv = kfsd::RpcVolume::mount_fd(sv[0], -1, dev, mo, &err);
    CHECK(rv != nullptr);
    if (rv) {
      int rc = 0;
      uint64_t ino;
      FsStat st;
      std::vector<FsDirEntry> ents;
      char buf[16];
      switch (mode) {
        case 2: case 4: rc = rv->readdir(1, &ents); CHECK(ents.empty()); break;
        case 3: rc = static_cast<int>(rv->read(2, 0, buf, 16)); break;
        case 6: rc = rv->stat(2, &st); break;
        default: rc = rv->lookup(1, "x", &ino); break;
      }
      if (!(rc < 0)) { std::fprintf(stderr, "  mode %d: rc=%d (expected negative)\n", mode, rc); g_fail++; }
      CHECK(!rv->alive() || rc < 0);
      CHECK_EQ(rc, -EPROTO);
      // 以降は -EPIPE (壊れた接続を使い続けない)
      CHECK_EQ(rv->lookup(1, "y", &ino), -EPIPE);
    }
    rv.reset();
    t.join();
  }
}

// kfsd が見つからない: ENOPKG (公開版の「プラグインが必要」の合図)
static void test_missing_exe() {
  std::fprintf(stderr, "missing_exe\n");
  int err = 0;
  auto dev = std::make_shared<MemoryDevice>(pattern(1, 4096));
  CHECK(kfsd::RpcVolume::mount("/nonexistent/kfsd", dev, {}, &err) == nullptr);
  CHECK_EQ(err, kfsd::kErrNoKfsd);
  CHECK(kfsd::RpcVolume::mount("", dev, {}, &err) == nullptr);
  CHECK_EQ(err, kfsd::kErrNoKfsd);
}

int main() {
  test_ops_equivalent();
  test_large_io();
  test_readdir_paging();
  test_device_errors();
  test_peer_failures();
  test_malicious_server();
  test_missing_exe();
  if (g_fail) { std::fprintf(stderr, "kfsd_proto: %d check(s) failed\n", g_fail); return 1; }
  std::fprintf(stderr, "kfsd_proto: OK\n");
  return 0;
}
