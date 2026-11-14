// SPDX-License-Identifier: GPL-2.0-or-later
// usbmem_fuse: core の FS API (FsVolume) を FUSE (libfuse3 の低水準 API) でホストにマウントして見せる。docs/xfstests.md
//
//   usbmem_fuse [-f] [-d] [-o opts] IMAGE MOUNTPOINT
//
// 主な用途: (1) xfstests (fstests) で自前実装の意味論を試す (FSTYP=fuse / FUSE_SUBTYP=.usbmem)、(2) PC で自前実装をマウントして中身を見る / 書く。
// 自前のオプション (-o): fstype=NAME (ext4 ext2 ext3 fat vfat exfat ntfs xfs btrfs f2fs iso9660 udf squashfs erofs。既定 auto = probe)、
//   ro / rw (既定 rw。rw を要求して core が書き込み不可 (writable()==false) と判断したら、黙って読み取り専用にせずマウントを失敗させる)、
//   offset=BYTES / size=BYTES (イメージの一部をボリュームとして使う。未実装)、quiet_setattr (下記)、uid=N / gid=N (stat に見せる所有者。既定 = 起動したユーザー)、
//   sync / dirsync (変更のたびに core の sync() を呼ぶ)。nosuid / noatime などの mount(8) の汎用フラグは無視する。
// それ以外の -o は libfuse に渡す (allow_other / default_permissions / fsname= / subtype= など)。subtype は既定では付けない (/proc/mounts の型は fuse。xfstests の FSTYP=fuse はこの型を前提にする)。
// 環境変数: USBMEM_FUSE_TRACE=1 (全操作を stderr に出す)、USBMEM_FUSE_FSCK="コマンド" (アンマウントでボリュームを閉じた後に `sh -c コマンド sh IMAGE` を実行し、
//   終了コードが 0 以外なら USBMEM_FUSE_FSCK_LOG (既定 stderr) に "FSCK-FAIL IMAGE" を書く。xfstests は FUSE の整合性検査を持たないのでその代わり)。
//
// core が持たない操作は、そのとおりに返す (対応しているふりをしない):
//   - chmod / chown / utimes (setattr の mode / uid / gid / atime / mtime): core に API が無い。既定は EOPNOTSUPP。
//     ただし要求が「今の値と同じ」なら成功する。-o quiet_setattr を付けると、FAT の quiet マウントと同じく黙って成功させる (値は変わらない。近似)。
//   - xattr / ACL / fallocate / ioctl / lseek(SEEK_DATA/HOLE) / copy_file_range / mknod (通常ファイル以外) / rename の EXCHANGE・WHITEOUT: 未対応 (ENOSYS / EOPNOTSUPP)。
//   - statfs: core に空き容量の API が無い。総容量 = イメージのサイズ、空き = 総容量の 95% からツリーのファイル大きさの合計を引いた見積もり (疎ファイル・メタデータは見ない近似)。
//   - 所有者・時刻: core は mode / nlink / size / mtime だけ返す。uid / gid は固定、atime / ctime は mtime と同じ。
// アダプター側の補完 (core の機能ではない): 開いたまま unlink / rename で上書きされたファイルを、最後に閉じるまで隠し名 (.usbmem_hidden_N) で残す (libfuse の hard_remove 無し相当)。
#define FUSE_USE_VERSION 312
#include <fuse3/fuse_lowlevel.h>

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <time.h>
#include <sys/wait.h>
#include <unistd.h>

#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "usbmem/block_device.h"
#include "usbmem/btrfs_volume.h"
#include "usbmem/devices.h"
#include "usbmem/erofs_volume.h"
#include "usbmem/exfat_volume.h"
#include "usbmem/ext2_volume.h"
#include "usbmem/ext4_volume.h"
#include "usbmem/f2fs_volume.h"
#include "usbmem/fat_volume.h"
#include "usbmem/fs_probe.h"
#include "usbmem/isofs_volume.h"
#include "usbmem/ntfs_volume.h"
#include "usbmem/squashfs_volume.h"
#include "usbmem/udf_volume.h"
#include "usbmem/xfs_volume.h"

using namespace usbmem;

namespace {

struct Cfg {
  std::string image, fstype = "auto";
  bool ro = false, quiet_setattr = false, foreground = false, sync_ops = false;
  uint64_t offset = 0, size = 0;
  uid_t uid = 0;
  gid_t gid = 0;
};

struct State {
  Cfg cfg;
  BlockDevicePtr dev;
  std::unique_ptr<FsVolume> vol;
  uint64_t root = 0, total_bytes = 0;
  bool trace = false, closed = false;
  std::unordered_map<uint64_t, int> opens;          // 実 ino -> 開いている数
  struct Hidden { uint64_t dir; std::string name; };
  std::unordered_map<uint64_t, Hidden> hidden;      // 実 ino -> 隠し名 (最後の close で unlink)
  std::unordered_map<uint64_t, uint64_t> alias;     // FUSE に渡した ino -> 隠し名への rename で変わった実 ino
  struct DirOpen { int count; FsStat st; };
  std::unordered_map<uint64_t, DirOpen> dir_opens;  // 開いているディレクトリ (実 ino) -> 開いた時点の stat。rename で上書き / rmdir された後の fstat に答えるため
  uint64_t hidden_seq = 0, dirty_ops = 0, dirty_bytes = 0;
};
State g;

void trace(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
void trace(const char* fmt, ...) {
  if (!g.trace) return;
  va_list ap;
  va_start(ap, fmt);
  vfprintf(stderr, fmt, ap);
  va_end(ap);
  fputc('\n', stderr);
}

int to_errno(int64_t rc) { return rc < 0 ? static_cast<int>(-rc) : static_cast<int>(rc); }

// FUSE の ino は 1 がルート。実 ino のルートと 1 を入れ替える (全単射)。
fuse_ino_t to_fuse(uint64_t real) {
  if (real == g.root) return FUSE_ROOT_ID;
  if (real == FUSE_ROOT_ID) return g.root;
  return real;
}
uint64_t to_real(fuse_ino_t f) {
  auto it = g.alias.find(f);
  if (it != g.alias.end()) return it->second;
  if (f == FUSE_ROOT_ID) return g.root;
  if (f == g.root) return FUSE_ROOT_ID;
  return f;
}

void fill_stat(uint64_t real, const FsStat& s, struct stat* st, bool unlinked = false) {
  memset(st, 0, sizeof *st);
  st->st_ino = to_fuse(real);
  st->st_mode = s.mode;
  st->st_nlink = unlinked ? 0 : (s.nlink ? s.nlink : 1);
  st->st_size = static_cast<off_t>(s.size);
  st->st_uid = g.cfg.uid;
  st->st_gid = g.cfg.gid;
  st->st_blksize = 4096;
  st->st_blocks = static_cast<blkcnt_t>((s.size + 511) / 512);
  st->st_atim.tv_sec = st->st_mtim.tv_sec = st->st_ctim.tv_sec = static_cast<time_t>(s.mtime);
}

bool is_dir(const FsStat& s) { return (s.mode & S_IFMT) == S_IFDIR; }
bool is_reg(const FsStat& s) { return (s.mode & S_IFMT) == S_IFREG; }

// 変更系の周期的な同期 (XFS などは変更をメモリに溜めてから sync / unmount でコミットするため、溜まりすぎないようにする)。
void maybe_sync(uint64_t bytes) {
  g.dirty_ops++;
  g.dirty_bytes += bytes;
  if (g.cfg.sync_ops || g.dirty_ops >= 512 || g.dirty_bytes >= (32u << 20)) {
    int rc = g.vol->sync();
    trace("auto-sync -> %d", rc);
    g.dirty_ops = g.dirty_bytes = 0;
  }
}

int fs_stat(uint64_t real, FsStat* s) {
  *s = FsStat{};
  return g.vol->stat(real, s);
}

int reply_entry(fuse_req_t req, uint64_t real) {
  FsStat s;
  int rc = fs_stat(real, &s);
  if (rc) return fuse_reply_err(req, to_errno(rc));
  struct fuse_entry_param e;
  memset(&e, 0, sizeof e);
  e.ino = to_fuse(real);
  fill_stat(real, s, &e.attr);
  return fuse_reply_entry(req, &e);
}

bool rw_ok(fuse_req_t req) {
  if (g.cfg.ro || !g.vol->writable()) { fuse_reply_err(req, EROFS); return false; }
  return true;
}

// ---- 開いたままの unlink: 隠し名へ rename して、最後の close で消す
std::string next_hidden_name() { return ".usbmem_hidden_" + std::to_string(++g.hidden_seq); }
bool is_hidden_name(const std::string& n) { return n.compare(0, 14, ".usbmem_hidden") == 0; }

// dir/name が通常ファイル (またはリンク) で、開かれていて、最後のリンクなら、隠し名へ rename して実 ino の追跡を移す。
// 0 = 何もしなかった / 隠した。負でなければ続行してよい。
int hide_if_open(uint64_t dir, const std::string& name, bool* hid) {
  *hid = false;
  uint64_t child;
  if (g.vol->lookup(dir, name, &child)) return 0;   // 無ければ呼び出し側のエラーに任せる
  FsStat s;
  if (fs_stat(child, &s)) return 0;
  if (is_dir(s) || s.nlink > 1) return 0;
  auto it = g.opens.find(child);
  if (it == g.opens.end() || it->second <= 0) return 0;
  if (g.hidden.count(child)) return 0;
  std::string hn = next_hidden_name();
  int rc = g.vol->rename(dir, name, dir, hn);
  if (rc) return rc;
  uint64_t now = child;
  if (g.vol->lookup(dir, hn, &now)) now = child;
  if (now != child) {
    g.opens[now] = g.opens[child];
    g.opens.erase(child);
    g.alias[to_fuse(child)] = now;   // カーネルが知っている ino (old) -> 今の実 ino
  }
  g.hidden[now] = {dir, hn};
  *hid = true;
  trace("hidden %s (ino %llu -> %llu)", hn.c_str(), (unsigned long long)child, (unsigned long long)now);
  return 0;
}

// ---- ops
void op_init(void*, struct fuse_conn_info* conn) { (void)conn; }

void shutdown_volume() {
  if (g.closed) return;
  g.closed = true;
  if (g.vol) {
    int rc = g.vol->sync();
    if (rc) fprintf(stderr, "usbmem_fuse: sync on unmount failed: %d\n", rc);
    g.vol.reset();   // 書き込みの最終コミット (XFS はデストラクタでもコミットする)
  }
  if (g.dev) { g.dev->flush(); g.dev->close(); g.dev.reset(); }
  const char* fsck = getenv("USBMEM_FUSE_FSCK");
  if (fsck && *fsck) {
    pid_t p = fork();
    if (p == 0) {
      execl("/bin/sh", "sh", "-c", fsck, "sh", g.cfg.image.c_str(), static_cast<char*>(nullptr));
      _exit(127);
    }
    int stt = 0;
    if (p > 0) waitpid(p, &stt, 0);
    if (p < 0 || !WIFEXITED(stt) || WEXITSTATUS(stt) != 0) {
      const char* lg = getenv("USBMEM_FUSE_FSCK_LOG");
      FILE* f = lg && *lg ? fopen(lg, "a") : nullptr;
      fprintf(f ? f : stderr, "FSCK-FAIL %s (status 0x%x)\n", g.cfg.image.c_str(), stt);
      if (f) fclose(f);
    }
  }
}

void op_destroy(void*) { shutdown_volume(); }

void op_lookup(fuse_req_t req, fuse_ino_t parent, const char* name) {
  uint64_t ino;
  int rc = g.vol->lookup(to_real(parent), name, &ino);
  trace("lookup %llu/%s -> %d", (unsigned long long)parent, name, rc);
  if (rc) { fuse_reply_err(req, to_errno(rc)); return; }
  reply_entry(req, ino);
}

void op_forget(fuse_req_t req, fuse_ino_t, uint64_t) { fuse_reply_none(req); }
void op_forget_multi(fuse_req_t req, size_t, struct fuse_forget_data*) { fuse_reply_none(req); }

void op_getattr(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info*) {
  FsStat s;
  uint64_t real = to_real(ino);
  int rc = fs_stat(real, &s);
  trace("getattr %llu -> %d", (unsigned long long)ino, rc);
  if (rc) {
    auto d = g.dir_opens.find(real);   // 開いたままのディレクトリが消された (rename で上書き / rmdir): 開いた時点の属性を nlink 0 で返す (Linux の通常の FS と同じ)
    if (d == g.dir_opens.end()) { fuse_reply_err(req, to_errno(rc)); return; }
    struct stat st;
    fill_stat(real, d->second.st, &st, true);
    fuse_reply_attr(req, &st, 0.0);
    return;
  }
  struct stat st;
  fill_stat(real, s, &st, g.hidden.count(real) != 0);   // 開いたまま unlink したファイル (隠し名) は nlink 0
  fuse_reply_attr(req, &st, 0.0);
}

void op_setattr(fuse_req_t req, fuse_ino_t ino, struct stat* attr, int to_set, struct fuse_file_info*) {
  uint64_t real = to_real(ino);
  FsStat s;
  int rc = fs_stat(real, &s);
  if (rc) { fuse_reply_err(req, to_errno(rc)); return; }
  trace("setattr %llu to_set=0x%x size=%lld", (unsigned long long)ino, to_set, (long long)attr->st_size);
  if (to_set & FUSE_SET_ATTR_SIZE) {
    if (!rw_ok(req)) return;
    if (is_dir(s)) { fuse_reply_err(req, EISDIR); return; }
    if (!is_reg(s)) { fuse_reply_err(req, EINVAL); return; }
    rc = g.vol->truncate(real, static_cast<uint64_t>(attr->st_size));
    trace("  truncate -> %d", rc);
    if (rc) { fuse_reply_err(req, to_errno(rc)); return; }
    maybe_sync(0);
  }
  if (!g.cfg.quiet_setattr) {
    bool bad = false;
    if ((to_set & FUSE_SET_ATTR_MODE) && ((attr->st_mode & 07777) != (s.mode & 07777))) bad = true;
    if ((to_set & FUSE_SET_ATTR_UID) && attr->st_uid != g.cfg.uid) bad = true;
    if ((to_set & FUSE_SET_ATTR_GID) && attr->st_gid != g.cfg.gid) bad = true;
    if (to_set & (FUSE_SET_ATTR_ATIME | FUSE_SET_ATTR_MTIME | FUSE_SET_ATTR_ATIME_NOW | FUSE_SET_ATTR_MTIME_NOW | FUSE_SET_ATTR_CTIME)) bad = true;
    if (bad) { fuse_reply_err(req, EOPNOTSUPP); return; }
  }
  rc = fs_stat(real, &s);
  if (rc) { fuse_reply_err(req, to_errno(rc)); return; }
  struct stat st;
  fill_stat(real, s, &st);
  fuse_reply_attr(req, &st, 0.0);
}

void op_readlink(fuse_req_t req, fuse_ino_t ino) {
  std::string t;
  int rc = g.vol->readlink(to_real(ino), &t);
  trace("readlink %llu -> %d", (unsigned long long)ino, rc);
  if (rc) fuse_reply_err(req, to_errno(rc));
  else fuse_reply_readlink(req, t.c_str());
}

void op_mknod(fuse_req_t req, fuse_ino_t parent, const char* name, mode_t mode, dev_t) {
  if (!rw_ok(req)) return;
  if ((mode & S_IFMT) != S_IFREG) { fuse_reply_err(req, EOPNOTSUPP); return; }
  uint64_t ino;
  int rc = g.vol->create(to_real(parent), name, mode & 07777, &ino);
  trace("mknod %s -> %d", name, rc);
  if (rc) { fuse_reply_err(req, to_errno(rc)); return; }
  maybe_sync(0);
  reply_entry(req, ino);
}

void op_mkdir(fuse_req_t req, fuse_ino_t parent, const char* name, mode_t mode) {
  if (!rw_ok(req)) return;
  uint64_t ino;
  int rc = g.vol->mkdir(to_real(parent), name, mode & 07777, &ino);
  trace("mkdir %s -> %d", name, rc);
  if (rc) { fuse_reply_err(req, to_errno(rc)); return; }
  maybe_sync(0);
  reply_entry(req, ino);
}

void op_unlink(fuse_req_t req, fuse_ino_t parent, const char* name) {
  if (!rw_ok(req)) return;
  uint64_t dir = to_real(parent);
  bool hid = false;
  int rc = hide_if_open(dir, name, &hid);
  if (!rc && !hid) rc = g.vol->unlink(dir, name);
  trace("unlink %s -> %d%s", name, rc, hid ? " (hidden)" : "");
  if (!rc) maybe_sync(0);
  fuse_reply_err(req, to_errno(rc));
}

void op_rmdir(fuse_req_t req, fuse_ino_t parent, const char* name) {
  if (!rw_ok(req)) return;
  int rc = g.vol->rmdir(to_real(parent), name);
  trace("rmdir %s -> %d", name, rc);
  if (!rc) maybe_sync(0);
  fuse_reply_err(req, to_errno(rc));
}

void op_symlink(fuse_req_t req, const char* link, fuse_ino_t parent, const char* name) {
  if (!rw_ok(req)) return;
  uint64_t ino;
  int rc = g.vol->symlink(to_real(parent), name, link, &ino);
  trace("symlink %s -> %d", name, rc);
  if (rc) { fuse_reply_err(req, to_errno(rc)); return; }
  maybe_sync(0);
  reply_entry(req, ino);
}

void op_rename(fuse_req_t req, fuse_ino_t parent, const char* name, fuse_ino_t newparent, const char* newname, unsigned int flags) {
  if (!rw_ok(req)) return;
  if (flags & ~static_cast<unsigned>(RENAME_NOREPLACE)) { fuse_reply_err(req, EINVAL); return; }   // EXCHANGE / WHITEOUT は未対応
  uint64_t od = to_real(parent), nd = to_real(newparent);
  if (flags & RENAME_NOREPLACE) {
    uint64_t tmp;
    if (g.vol->lookup(nd, newname, &tmp) == 0) { fuse_reply_err(req, EEXIST); return; }
  }
  bool hid = false;
  int rc = hide_if_open(nd, newname, &hid);   // 開いたまま上書きされる行き先は隠す (rename が行き先の inode を解放するため)
  if (!rc) {
    rc = g.vol->rename(od, name, nd, newname);
  }
  trace("rename %s -> %s -> %d", name, newname, rc);
  if (!rc) maybe_sync(0);
  fuse_reply_err(req, to_errno(rc));
}

void op_link(fuse_req_t req, fuse_ino_t ino, fuse_ino_t newparent, const char* newname) {
  if (!rw_ok(req)) return;
  uint64_t real = to_real(ino);
  int rc = g.vol->link(to_real(newparent), newname, real);
  trace("link %s -> %d", newname, rc);
  if (rc) { fuse_reply_err(req, to_errno(rc)); return; }
  maybe_sync(0);
  reply_entry(req, real);
}

void op_open(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi) {
  uint64_t real = to_real(ino);
  if ((fi->flags & O_ACCMODE) != O_RDONLY || (fi->flags & O_TRUNC)) {
    if (!rw_ok(req)) return;
  }
  FsStat s;
  int rc = fs_stat(real, &s);
  if (rc) { fuse_reply_err(req, to_errno(rc)); return; }
  if (is_dir(s)) { fuse_reply_err(req, EISDIR); return; }
  g.opens[real]++;
  trace("open %llu flags=0x%x", (unsigned long long)ino, fi->flags);
  fi->fh = 0;
  fuse_reply_open(req, fi);
}

void op_create(fuse_req_t req, fuse_ino_t parent, const char* name, mode_t mode, struct fuse_file_info* fi) {
  if (!rw_ok(req)) return;
  uint64_t dir = to_real(parent), ino;
  int rc = g.vol->create(dir, name, mode & 07777, &ino);
  trace("create %s -> %d", name, rc);
  if (rc) { fuse_reply_err(req, to_errno(rc)); return; }
  maybe_sync(0);
  FsStat s;
  rc = fs_stat(ino, &s);
  if (rc) { fuse_reply_err(req, to_errno(rc)); return; }
  struct fuse_entry_param e;
  memset(&e, 0, sizeof e);
  e.ino = to_fuse(ino);
  fill_stat(ino, s, &e.attr);
  g.opens[ino]++;
  fi->fh = 0;
  fuse_reply_create(req, &e, fi);
}

void op_release(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info*) {
  uint64_t real = to_real(ino);
  auto it = g.opens.find(real);
  if (it != g.opens.end() && --it->second <= 0) {
    g.opens.erase(it);
    auto h = g.hidden.find(real);
    if (h != g.hidden.end()) {
      int rc = g.vol->unlink(h->second.dir, h->second.name);
      trace("release: unlink hidden %s -> %d", h->second.name.c_str(), rc);
      g.hidden.erase(h);
      g.alias.erase(ino);
      maybe_sync(0);
    }
  }
  fuse_reply_err(req, 0);
}

void op_flush(fuse_req_t req, fuse_ino_t, struct fuse_file_info*) { fuse_reply_err(req, 0); }

void op_fsync(fuse_req_t req, fuse_ino_t, int, struct fuse_file_info*) {
  int rc = g.cfg.ro ? 0 : g.vol->sync();
  trace("fsync -> %d", rc);
  g.dirty_ops = g.dirty_bytes = 0;
  fuse_reply_err(req, to_errno(rc));
}

void op_read(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info*) {
  std::vector<char> buf(size);
  int64_t n = g.vol->read(to_real(ino), static_cast<uint64_t>(off), buf.data(), size);
  trace("read %llu off=%lld len=%zu -> %lld", (unsigned long long)ino, (long long)off, size, (long long)n);
  if (n < 0) { fuse_reply_err(req, to_errno(n)); return; }
  fuse_reply_buf(req, buf.data(), static_cast<size_t>(n));
}

void op_write(fuse_req_t req, fuse_ino_t ino, const char* buf, size_t size, off_t off, struct fuse_file_info*) {
  if (!rw_ok(req)) return;
  size_t done = 0;
  uint64_t real = to_real(ino);
  while (done < size) {   // 短い書き込みは続ける (core は全量か負の errno を返す想定だが、念のため)
    int64_t n = g.vol->write(real, static_cast<uint64_t>(off) + done, buf + done, size - done);
    trace("write %llu off=%lld len=%zu -> %lld", (unsigned long long)ino, (long long)off + (long long)done, size - done, (long long)n);
    if (n < 0) {
      if (done) break;
      fuse_reply_err(req, to_errno(n));
      return;
    }
    if (n == 0) break;
    done += static_cast<size_t>(n);
  }
  maybe_sync(done);
  fuse_reply_write(req, done);
}

struct DirHandle { std::vector<FsDirEntry> ents; uint64_t real = 0; };

void load_dir(uint64_t dir, std::vector<FsDirEntry>* out, int* rc) {
  std::vector<FsDirEntry> raw;
  *rc = g.vol->readdir(dir, &raw);
  out->clear();
  if (*rc) return;
  // "." と ".." は core の readdir が返すものを使う (無い FS だけ補う。".." の lookup はしない: ext4 のルートで ".." を引くと FS エラー扱い (journal abort) になった)
  bool has_dot = false, has_dotdot = false;
  for (auto& e : raw) { if (e.name == ".") has_dot = true; else if (e.name == "..") has_dotdot = true; }
  if (!has_dot) out->push_back({".", dir, DT_DIR});
  if (!has_dotdot) out->push_back({"..", dir, DT_DIR});
  for (auto& e : raw) {
    if (is_hidden_name(e.name)) continue;
    out->push_back(e);
  }
}

void op_opendir(fuse_req_t req, fuse_ino_t ino, struct fuse_file_info* fi) {
  uint64_t real = to_real(ino);
  FsStat s;
  int rc = fs_stat(real, &s);
  if (rc) { fuse_reply_err(req, to_errno(rc)); return; }
  if (!is_dir(s)) { fuse_reply_err(req, ENOTDIR); return; }
  auto* h = new DirHandle;
  h->real = real;
  load_dir(real, &h->ents, &rc);
  trace("opendir %llu -> %d (%zu entries)", (unsigned long long)ino, rc, h->ents.size());
  if (rc) { delete h; fuse_reply_err(req, to_errno(rc)); return; }
  auto& d = g.dir_opens[real];
  d.count++;
  d.st = s;
  fi->fh = reinterpret_cast<uintptr_t>(h);
  fuse_reply_open(req, fi);
}

void op_readdir(fuse_req_t req, fuse_ino_t ino, size_t size, off_t off, struct fuse_file_info* fi) {
  auto* h = reinterpret_cast<DirHandle*>(fi->fh);
  if (!h) { fuse_reply_err(req, EBADF); return; }
  int rc = 0;
  if (off == 0) {   // rewinddir: 読み直す
    load_dir(to_real(ino), &h->ents, &rc);
    if (rc) { fuse_reply_err(req, to_errno(rc)); return; }
  }
  std::vector<char> buf(size);
  size_t used = 0;
  for (size_t i = static_cast<size_t>(off); i < h->ents.size(); i++) {
    const FsDirEntry& e = h->ents[i];
    struct stat st;
    memset(&st, 0, sizeof st);
    st.st_ino = to_fuse(e.ino);
    unsigned dt = e.dtype;
    if (dt == 0 || dt == DT_UNKNOWN) {
      FsStat s;
      dt = fs_stat(e.ino, &s) == 0 ? ((s.mode & S_IFMT) >> 12) : DT_REG;
    }
    st.st_mode = static_cast<mode_t>(dt << 12);
    size_t need = fuse_add_direntry(req, nullptr, 0, e.name.c_str(), &st, 0);
    if (used + need > size) break;
    fuse_add_direntry(req, buf.data() + used, size - used, e.name.c_str(), &st, static_cast<off_t>(i + 1));
    used += need;
  }
  fuse_reply_buf(req, buf.data(), used);
}

void op_releasedir(fuse_req_t req, fuse_ino_t, struct fuse_file_info* fi) {
  auto* h = reinterpret_cast<DirHandle*>(fi->fh);
  if (h) {
    auto d = g.dir_opens.find(h->real);
    if (d != g.dir_opens.end() && --d->second.count <= 0) g.dir_opens.erase(d);
  }
  delete h;
  fuse_reply_err(req, 0);
}

void op_fsyncdir(fuse_req_t req, fuse_ino_t, int, struct fuse_file_info*) {
  int rc = g.cfg.ro ? 0 : g.vol->sync();
  fuse_reply_err(req, to_errno(rc));
}

// 使用量の見積もり: core に空き容量の API が無いので、ツリーを歩いて通常ファイル / シンボリックリンクの大きさ (4 KiB 単位に切り上げ) を足す。
// 疎ファイル・メタデータ・ジャーナルは分からない (近似)。大きなツリー (20 万エントリ超 / 3 秒超) は諦めて空き 0 (不明) を返す。
bool estimate_used(uint64_t* used) {
  struct timespec t0;
  clock_gettime(CLOCK_MONOTONIC, &t0);
  std::vector<uint64_t> stack = {g.root};
  std::unordered_map<uint64_t, bool> seen;   // ハードリンクの重複を避ける
  uint64_t total = 0, entries = 0;
  while (!stack.empty()) {
    uint64_t d = stack.back();
    stack.pop_back();
    std::vector<FsDirEntry> ents;
    if (g.vol->readdir(d, &ents)) continue;
    for (auto& e : ents) {
      if (e.name == "." || e.name == "..") continue;
      if (++entries > 200000) return false;
      struct timespec t1;
      clock_gettime(CLOCK_MONOTONIC, &t1);
      if (t1.tv_sec - t0.tv_sec > 3) return false;
      FsStat s;
      if (fs_stat(e.ino, &s)) continue;
      if (is_dir(s)) { stack.push_back(e.ino); continue; }
      if (!is_reg(s) && (s.mode & S_IFMT) != S_IFLNK) continue;
      if (s.nlink > 1 && seen.count(e.ino)) continue;
      seen[e.ino] = true;
      total += (s.size + 4095) / 4096 * 4096;
    }
  }
  *used = total;
  return true;
}

void op_statfs(fuse_req_t req, fuse_ino_t) {
  struct statvfs sv;
  memset(&sv, 0, sizeof sv);
  sv.f_bsize = sv.f_frsize = 4096;
  sv.f_blocks = g.total_bytes / 4096;
  sv.f_namemax = 255;
  uint64_t used = 0;
  if (!g.cfg.ro && estimate_used(&used)) {
    uint64_t reserve = sv.f_blocks / 20;   // メタデータ分の見積もり 5%
    uint64_t u = used / 4096 + reserve;
    sv.f_bfree = sv.f_bavail = u >= sv.f_blocks ? 0 : sv.f_blocks - u;
  }
  fuse_reply_statfs(req, &sv);
}

void op_access(fuse_req_t req, fuse_ino_t, int) { fuse_reply_err(req, 0); }   // 権限は見ない (core に所有者・権限の情報が無い)

void op_fallocate(fuse_req_t req, fuse_ino_t, int, off_t, off_t, struct fuse_file_info*) { fuse_reply_err(req, EOPNOTSUPP); }

// ---- マウント
std::unique_ptr<FsVolume> mount_volume(BlockDevicePtr dev, const std::string& type, bool rw, int* err, std::string* why) {
  *err = 0;
  auto fail = [&](const char* w) { if (why) *why = w; };
#define TRY(CLS)                                              \
  rw ? CLS::mount_rw(dev, err) : CLS::mount(dev, err)
  std::unique_ptr<FsVolume> v;
  if (type == "ext4" || type == "ext3") v = TRY(Ext4Volume);
  else if (type == "ext2") v = TRY(Ext2Volume);
  else if (type == "fat" || type == "vfat" || type == "fat12" || type == "fat16" || type == "fat32") v = TRY(FatVolume);
  else if (type == "exfat") v = TRY(ExfatVolume);
  else if (type == "ntfs") v = TRY(NtfsVolume);
  else if (type == "xfs") v = TRY(XfsVolume);
  else if (type == "btrfs") v = TRY(BtrfsVolume);
  else if (type == "f2fs") v = TRY(F2fsVolume);
  else if (type == "iso9660" || type == "isofs") v = IsofsVolume::mount(dev, err);
  else if (type == "udf") v = UdfVolume::mount(dev, err);
  else if (type == "squashfs") v = SquashfsVolume::mount(dev, err);
  else if (type == "erofs") v = ErofsVolume::mount(dev, err);
  else { fail("unknown fstype"); *err = EINVAL; return nullptr; }
  if (!v) fail("mount failed");
  return v;
}

std::string auto_type(BlockDevice& dev) {
  Status io;
  FsType t = probe_filesystem(dev, 0, dev.size(), &io);
  switch (t) {
    case FsType::Ntfs: return "ntfs";
    case FsType::Exfat: return "exfat";
    case FsType::Fat12: case FsType::Fat16: case FsType::Fat32: return "fat";
    case FsType::Ext2: return "ext2";
    case FsType::Ext3: case FsType::Ext4: return "ext4";
    case FsType::Iso9660: return "iso9660";
    case FsType::Udf: return "udf";
    case FsType::Btrfs: return "btrfs";
    case FsType::Xfs: return "xfs";
    case FsType::F2fs: return "f2fs";
    case FsType::Squashfs: return "squashfs";
    case FsType::Erofs: return "erofs";
    default: return "";
  }
}

std::string read_only_reason(FsVolume* v) {
  if (auto* x = dynamic_cast<XfsVolume*>(v)) return x->read_only_reason();
  if (auto* b = dynamic_cast<BtrfsVolume*>(v)) return b->read_only_reason();
  if (auto* f = dynamic_cast<F2fsVolume*>(v)) return "write_refusal errno " + std::to_string(f->write_refusal());
  return "(no reason reported by the driver)";
}

void usage() {
  fprintf(stderr,
          "usage: usbmem_fuse [-f] [-d] [-o opts] IMAGE MOUNTPOINT\n"
          "  -o fstype=auto|ext4|ext2|ext3|fat|exfat|ntfs|xfs|btrfs|f2fs|iso9660|udf|squashfs|erofs, ro, rw, quiet_setattr, uid=N, gid=N,\n"
          "     and any libfuse mount option (allow_other, default_permissions, fsname=..., subtype=...)\n"
          "  env: USBMEM_FUSE_TRACE=1, USBMEM_FUSE_FSCK='cmd using $1 = image', USBMEM_FUSE_FSCK_LOG=file\n");
}

}  // namespace

int main(int argc, char** argv) {
  Cfg& c = g.cfg;
  c.uid = getuid();
  c.gid = getgid();
  std::vector<std::string> fuse_opts;
  bool debug = false;
  std::vector<std::string> pos;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a == "-f") c.foreground = true;
    else if (a == "-d") { debug = true; c.foreground = true; }
    else if (a == "-h" || a == "--help") { usage(); return 0; }
    else if (a == "-o" || a.compare(0, 2, "-o") == 0) {
      std::string v = a == "-o" ? (i + 1 < argc ? argv[++i] : "") : a.substr(2);
      size_t p = 0;
      while (p <= v.size()) {
        size_t q = v.find(',', p);
        if (q == std::string::npos) q = v.size();
        std::string o = v.substr(p, q - p);
        p = q + 1;
        if (o.empty()) continue;
        if (o.compare(0, 7, "fstype=") == 0) c.fstype = o.substr(7);
        else if (o == "ro") { c.ro = true; fuse_opts.push_back(o); }
        else if (o == "rw") c.ro = false;
        else if (o == "sync" || o == "dirsync") c.sync_ops = true;   // 変更のたびに core の sync() を呼ぶ
        else if (o == "nosuid" || o == "nodev" || o == "noexec" || o == "suid" || o == "dev" || o == "exec" || o == "atime" || o == "noatime" || o == "relatime" ||
                 o == "strictatime" || o == "nodiratime" || o == "diratime" || o == "lazytime" || o == "defaults" || o == "async" || o == "user" || o == "nouser" ||
                 o == "auto" || o == "noauto" || o == "nofail" || o == "_netdev" || o == "silent" || o == "loud" || o == "mand" || o == "nomand") {}   // mount(8) の汎用フラグ。無視
        else if (o == "quiet_setattr") c.quiet_setattr = true;
        else if (o.compare(0, 4, "uid=") == 0) c.uid = static_cast<uid_t>(strtoul(o.c_str() + 4, nullptr, 0));
        else if (o.compare(0, 4, "gid=") == 0) c.gid = static_cast<gid_t>(strtoul(o.c_str() + 4, nullptr, 0));
        else if (o.compare(0, 7, "offset=") == 0 || o.compare(0, 5, "size=") == 0) { fprintf(stderr, "usbmem_fuse: %s is not implemented\n", o.c_str()); return 2; }
        else {
          fuse_opts.push_back(o);
        }
      }
    } else if (a == "-t" || a == "-N") { i++; }   // mount(8) のヘルパー形式の残り (無視)
    else if (!a.empty() && a[0] == '-' && a.size() > 1) { /* -s -n -v など: 無視 */ }
    else pos.push_back(a);
  }
  if (pos.size() != 2) { usage(); return 2; }
  char rp[PATH_MAX];
  c.image = realpath(pos[0].c_str(), rp) ? rp : pos[0];
  std::string mnt = realpath(pos[1].c_str(), rp) ? rp : pos[1];
  g.trace = getenv("USBMEM_FUSE_TRACE") && *getenv("USBMEM_FUSE_TRACE") == '1';

  // デーモン化: 子がマウントして、準備ができた (または失敗した) ことを親に知らせる (先頭 1 バイト = 結果、残り = 失敗の理由)。
  // 親は mount(8) のヘルパーとして理由を stderr に出し、終了コードを返す。子の stdout / stderr は最初にログへ向ける
  // (互換層がマウント時に stdout に出す診断が mount の出力に混ざると、xfstests の出力比較が壊れる)。
  int pfd[2] = {-1, -1};
  if (!c.foreground) {
    if (pipe(pfd)) { perror("pipe"); return 1; }
    pid_t pid = fork();
    if (pid < 0) { perror("fork"); return 1; }
    if (pid > 0) {
      close(pfd[1]);
      std::string msg;
      char b[512];
      ssize_t n;
      while ((n = read(pfd[0], b, sizeof b)) > 0) msg.append(b, static_cast<size_t>(n));
      if (msg.empty()) { fprintf(stderr, "usbmem_fuse: daemon exited unexpectedly\n"); return 1; }
      if (msg.size() > 1) fprintf(stderr, "%s", msg.c_str() + 1);
      return msg[0] == 0 ? 0 : 1;
    }
    close(pfd[0]);
    setsid();
    const char* lg = getenv("USBMEM_FUSE_LOG");
    int nul = open(lg && *lg ? lg : "/dev/null", O_WRONLY | O_CREAT | O_APPEND, 0644);
    if (nul >= 0) { dup2(nul, 1); dup2(nul, 2); if (nul > 2) close(nul); }
    int in = open("/dev/null", O_RDONLY);
    if (in >= 0) { dup2(in, 0); if (in > 2) close(in); }
  }
  auto notify = [&](char r, const std::string& msg = "") {
    if (pfd[1] >= 0) {
      std::string m(1, r);
      m += msg;
      ssize_t w = write(pfd[1], m.data(), m.size());
      (void)w;
      close(pfd[1]);
      pfd[1] = -1;
    }
  };
  auto die = [&](const std::string& msg) -> int {
    fprintf(stderr, "usbmem_fuse: %s\n", msg.c_str());
    notify(1, "usbmem_fuse: " + msg + "\n");
    return 1;
  };

  Status st = RawImageDevice::open_path(c.image, c.ro, 512, &g.dev);
  if (!st.ok()) return die("cannot open " + c.image + " (" + errc_name(st.code) + ", errno " + std::to_string(st.sys_errno) + ")");
  g.total_bytes = g.dev->size();
  std::string type = c.fstype == "auto" ? auto_type(*g.dev) : c.fstype;
  if (type.empty()) return die("cannot detect the filesystem of " + c.image + " (use -o fstype=)");
  int err = 0;
  std::string why;
  bool rw = !c.ro;
  bool ro_only_fs = type == "iso9660" || type == "isofs" || type == "udf" || type == "squashfs" || type == "erofs";
  if (ro_only_fs) { rw = false; c.ro = true; fuse_opts.push_back("ro"); }
  g.vol = mount_volume(g.dev, type, rw, &err, &why);
  if (!g.vol) return die("mount " + type + " failed: " + why + " (errno " + std::to_string(err) + ": " + strerror(err) + ")");
  if (rw && !g.vol->writable()) {
    std::string why2 = read_only_reason(g.vol.get());
    if (type == "ext2" || type == "ext4" || type == "ext3") {   // 理由を報告しないドライバーのために、s_state を見せる (0x1 = クリーン、0x2 = 汚れ / 未アンマウント、0x4 = エラー)
      uint8_t sb[2] = {0, 0};
      if (g.dev->read_at(1024 + 0x3A, sb, 2).ok()) { char b[64]; snprintf(b, sizeof b, " [s_state=0x%x]", sb[0] | (sb[1] << 8)); why2 += b; }
    }
    return die("the " + type + " driver opened the volume read-only: " + why2 + " (use -o ro to mount read-only)");
  }
  g.root = g.vol->root();

  std::string fsname = c.image;
  bool has_fsname = false;
  for (auto& o : fuse_opts) if (o.compare(0, 7, "fsname=") == 0) has_fsname = true;
  if (!has_fsname) fuse_opts.push_back("fsname=" + fsname);
  std::string optstr;
  for (auto& o : fuse_opts) { if (!optstr.empty()) optstr += ','; optstr += o; }

  std::vector<std::string> av = {"usbmem_fuse"};
  if (debug) av.push_back("-d");
  av.push_back("-o");
  av.push_back(optstr);
  std::vector<char*> avp;
  for (auto& s : av) avp.push_back(const_cast<char*>(s.c_str()));
  struct fuse_args args = FUSE_ARGS_INIT(static_cast<int>(avp.size()), avp.data());

  struct fuse_lowlevel_ops ops;
  memset(&ops, 0, sizeof ops);
  ops.init = op_init;
  ops.destroy = op_destroy;
  ops.lookup = op_lookup;
  ops.forget = op_forget;
  ops.forget_multi = op_forget_multi;
  ops.getattr = op_getattr;
  ops.setattr = op_setattr;
  ops.readlink = op_readlink;
  ops.mknod = op_mknod;
  ops.mkdir = op_mkdir;
  ops.unlink = op_unlink;
  ops.rmdir = op_rmdir;
  ops.symlink = op_symlink;
  ops.rename = op_rename;
  ops.link = op_link;
  ops.open = op_open;
  ops.read = op_read;
  ops.write = op_write;
  ops.flush = op_flush;
  ops.release = op_release;
  ops.fsync = op_fsync;
  ops.opendir = op_opendir;
  ops.readdir = op_readdir;
  ops.releasedir = op_releasedir;
  ops.fsyncdir = op_fsyncdir;
  ops.statfs = op_statfs;
  ops.access = op_access;
  ops.create = op_create;
  ops.fallocate = op_fallocate;

  struct fuse_session* se = fuse_session_new(&args, &ops, sizeof ops, nullptr);
  if (!se) return die("fuse_session_new failed");
  if (fuse_set_signal_handlers(se)) { fuse_session_destroy(se); return die("fuse_set_signal_handlers failed"); }
  if (fuse_session_mount(se, mnt.c_str())) {
    fuse_remove_signal_handlers(se);
    fuse_session_destroy(se);
    return die("fuse_session_mount " + mnt + " failed");
  }
  fuse_opt_free_args(&args);
  notify(0);
  int ret = fuse_session_loop(se);
  fuse_session_unmount(se);
  shutdown_volume();   // 通常は destroy で済んでいる (シグナルで抜けたときの保険)
  fuse_remove_signal_handlers(se);
  fuse_session_destroy(se);
  return ret ? 1 : 0;
}
