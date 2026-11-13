// SPDX-License-Identifier: Apache-2.0
// 本物の Linux カーネルに F2FS を書かせるための VM 用 init (initramfs の /init。static にビルドする)。
// 実行内容は /job.txt の行を順に実行する (tools/f2fs_vm/gen_k_fixtures.py が作る):
//   LOADMOD <path>                     カーネルモジュールを読み込む
//   MOUNT <dev> <options>              /mnt に f2fs をマウント (options は mount -o の文字列。空でもよい)
//   COPY                               /tree を /mnt にコピー (ハードリンク・シンボリックリンク・mtime を保つ。ゼロだけの 4 KiB は穴にする)
//   SPECIAL <kind> <path> <mtime>      kind = p (FIFO) / s (ソケット) / c (キャラクタ 1:3) / b (ブロック 8:1)
//   W <dest> <offset> <srcfile>        dest の offset に srcfile の中身を書く (なければ作る)
//   T <dest> <size>                    dest を size にする (ftruncate)
//   MT <path> <mtime>                  mtime を設定
//   APPENDFSYNC <path> <srcfile>       既存ファイルの末尾に書いて fsync (チェックポイントなしで fsync マークのノードが書かれる)
//   NEWFSYNC <path> <srcfile>          新しいファイルを作って書き、fsync
//   SYNC                               sync() (チェックポイントを書く)
//   UMOUNT                             /mnt をアンマウント (きれいに終える)
//   POWEROFF                           すぐに電源を切る (アンマウントも sync もしない)
//   OPS <file>                         操作列 (tools/ops_ref.py と同じ書式) を /mnt に POSIX 操作として実行 (失敗は "OPS-FAIL:" をシリアルに出す)
//   DUMPTREE                           /mnt 全体を "TREE|d 0 - パス" / "TREE|f サイズ sha256 パス" / "TREE|l 0 リンク先 パス" でシリアルに出す (tools/ops_ref.py --tree と同じ書式)
//   MOUNT の options が "ro" なら読み取り専用でマウントする
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/reboot.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

static void die(const char *what) { fprintf(stderr, "INIT-FAIL: %s: %s\n", what, strerror(errno)); fflush(stderr); syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_POWER_OFF, 0); for (;;) pause(); }

#define MAXLINKS 4096
static struct { dev_t dev; ino_t ino; char path[1024]; } links[MAXLINKS];
static int nlinks;

static void set_mtime(const char *path, long t, int nofollow) {
  struct timespec ts[2] = {{t, 0}, {t, 0}};
  if (utimensat(AT_FDCWD, path, ts, nofollow ? AT_SYMLINK_NOFOLLOW : 0) != 0) die(path);
}

static void copy_file(const char *src, const char *dst, const struct stat *st) {
  int in = open(src, O_RDONLY);
  if (in < 0) die(src);
  int out = open(dst, O_WRONLY | O_CREAT | O_EXCL, st->st_mode & 07777);
  if (out < 0) die(dst);
  static char buf[4096];
  off_t off = 0;
  ssize_t n;
  while ((n = read(in, buf, sizeof buf)) > 0) {
    int zero = 1;
    for (ssize_t i = 0; i < n; i++) if (buf[i]) { zero = 0; break; }
    if (!zero) { if (pwrite(out, buf, n, off) != n) die(dst); }
    off += n;
  }
  if (ftruncate(out, st->st_size) != 0) die(dst);
  close(in); close(out);
  set_mtime(dst, st->st_mtime, 0);
}

static void copy_tree(const char *src, const char *dst) {
  DIR *d = opendir(src);
  if (!d) die(src);
  struct dirent *e;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char s[2048], t[2048];
    snprintf(s, sizeof s, "%s/%s", src, e->d_name);
    snprintf(t, sizeof t, "%s/%s", dst, e->d_name);
    struct stat st;
    if (lstat(s, &st) != 0) die(s);
    if (S_ISDIR(st.st_mode)) {
      if (mkdir(t, st.st_mode & 07777) != 0) die(t);
      copy_tree(s, t);
      set_mtime(t, st.st_mtime, 0);
    } else if (S_ISLNK(st.st_mode)) {
      char tgt[4096];
      ssize_t n = readlink(s, tgt, sizeof tgt - 1);
      if (n < 0) die(s);
      tgt[n] = 0;
      if (symlink(tgt, t) != 0) die(t);
      set_mtime(t, st.st_mtime, 1);
    } else if (S_ISREG(st.st_mode)) {
      int found = -1;
      if (st.st_nlink > 1) for (int i = 0; i < nlinks; i++) if (links[i].dev == st.st_dev && links[i].ino == st.st_ino) { found = i; break; }
      if (found >= 0) { if (link(links[found].path, t) != 0) die(t); }
      else {
        copy_file(s, t, &st);
        if (st.st_nlink > 1 && nlinks < MAXLINKS) { links[nlinks].dev = st.st_dev; links[nlinks].ino = st.st_ino; snprintf(links[nlinks].path, sizeof links[nlinks].path, "%s", t); nlinks++; }
      }
    }
  }
  closedir(d);
}

static void write_piece(const char *dest, long off, const char *srcfile, int fsync_it, int append) {
  int in = open(srcfile, O_RDONLY);
  if (in < 0) die(srcfile);
  static char buf[1 << 16];
  ssize_t n = read(in, buf, sizeof buf);
  if (n < 0) die(srcfile);
  close(in);
  int out = open(dest, O_WRONLY | O_CREAT | (append ? O_APPEND : 0), 0644);
  if (out < 0) die(dest);
  if (append) { if (write(out, buf, n) != n) die(dest); }
  else if (pwrite(out, buf, n, off) != n) die(dest);
  if (fsync_it && fsync(out) != 0) die("fsync");
  close(out);
}

static void load_module(const char *path) {
  int fd = open(path, O_RDONLY);
  if (fd < 0) die(path);
  if (syscall(SYS_finit_module, fd, "", 0) != 0 && errno != EEXIST) die(path);
  close(fd);
}

// ---- 書き込みテスト用 (docs/f2fs-write.md): 操作列の実行 (OPS) とツリーのダンプ (DUMPTREE)。書式は tools/ops_ref.py と同じ。
typedef struct { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t n; } sha_t;
static const uint32_t K256[64] = {
  0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
  0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
  0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
  0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_block(sha_t *s, const uint8_t *p) {
  uint32_t w[64], a, b, c, d, e, f, g, h;
  for (int i = 0; i < 16; i++) w[i] = (uint32_t)p[4*i] << 24 | p[4*i+1] << 16 | p[4*i+2] << 8 | p[4*i+3];
  for (int i = 16; i < 64; i++) { uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3), s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10); w[i] = w[i-16] + s0 + w[i-7] + s1; }
  a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K256[i] + w[i];
    uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}
static void sha_init(sha_t *s) {
  static const uint32_t iv[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19};
  memcpy(s->h, iv, sizeof iv); s->len = 0; s->n = 0;
}
static void sha_update(sha_t *s, const uint8_t *p, size_t n) {
  s->len += n;
  while (n) { size_t k = 64 - s->n; if (k > n) k = n; memcpy(s->buf + s->n, p, k); s->n += k; p += k; n -= k; if (s->n == 64) { sha_block(s, s->buf); s->n = 0; } }
}
static void sha_hex(sha_t *s, char *out) {
  uint64_t bits = s->len * 8; uint8_t pad = 0x80; sha_update(s, &pad, 1); pad = 0;
  while (s->n != 56) sha_update(s, &pad, 1);
  uint8_t lb[8]; for (int i = 0; i < 8; i++) lb[i] = (uint8_t)(bits >> (56 - 8 * i));
  sha_update(s, lb, 8);
  for (int i = 0; i < 8; i++) sprintf(out + 8 * i, "%08x", s->h[i]);
}
static void dump_tree(const char *root, const char *rel) {
  char dir[2048]; snprintf(dir, sizeof dir, "%s%s%s", root, rel[0] ? "/" : "", rel);
  DIR *d = opendir(dir);
  if (!d) die(dir);
  struct dirent *e;
  while ((e = readdir(d))) {
    if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
    char r[2048], p[4096];
    snprintf(r, sizeof r, "%s%s%s", rel, rel[0] ? "/" : "", e->d_name);
    snprintf(p, sizeof p, "%s/%s", root, r);
    struct stat st;
    if (lstat(p, &st) != 0) die(p);
    if (S_ISDIR(st.st_mode)) { fprintf(stderr, "TREE|d 0 - %s\n", r); dump_tree(root, r); }
    else if (S_ISLNK(st.st_mode)) { char t[4096]; ssize_t n = readlink(p, t, sizeof t - 1); if (n < 0) die(p); t[n] = 0; fprintf(stderr, "TREE|l 0 %s %s\n", t, r); }
    else if (S_ISREG(st.st_mode)) {
      int fd = open(p, O_RDONLY); if (fd < 0) die(p);
      sha_t s; sha_init(&s); static uint8_t buf[1 << 16]; ssize_t n; uint64_t tot = 0;
      while ((n = read(fd, buf, sizeof buf)) > 0) { sha_update(&s, buf, n); tot += n; }
      if (n < 0) die(p);
      close(fd);
      char hx[65]; sha_hex(&s, hx);
      fprintf(stderr, "TREE|f %llu %s %s\n", (unsigned long long)tot, hx, r);
    }
  }
  closedir(d);
}
static void run_ops(const char *file) {
  FILE *f = fopen(file, "r");
  if (!f) die(file);
  static char line[8192];
  int n = 0;
  while (fgets(line, sizeof line, f)) {
    line[strcspn(line, "\n")] = 0;
    char *tok[8]; int nt = 0;
    for (char *t = strtok(line, " "); t && nt < 8; t = strtok(NULL, " ")) tok[nt++] = t;
    if (nt == 0 || tok[0][0] == '#') continue;
    char a[4096], b[4096];
    snprintf(a, sizeof a, "/mnt/%s", nt > 1 ? tok[1] + (tok[1][0] == '/') : "");
    snprintf(b, sizeof b, "/mnt/%s", nt > 2 ? tok[2] + (tok[2][0] == '/') : "");
    const char *op = tok[0]; int rc = 0; n++;
    if (!strcmp(op, "mkdir")) rc = mkdir(a, 0755);
    else if (!strcmp(op, "create")) { int fd = open(a, O_WRONLY | O_CREAT | O_EXCL, 0644); rc = fd < 0 ? -1 : close(fd); }
    else if (!strcmp(op, "unlink")) rc = unlink(a);
    else if (!strcmp(op, "rmdir")) rc = rmdir(a);
    else if (!strcmp(op, "rename")) rc = rename(a, b);
    else if (!strcmp(op, "truncate")) rc = truncate(a, atoll(tok[2]));
    else if (!strcmp(op, "symlink")) rc = symlink(tok[2], a);
    else if (!strcmp(op, "link")) rc = link(b, a);
    else if (!strcmp(op, "write")) {
      long long off = atoll(tok[2]), len = atoll(tok[3]); int seed = atoi(tok[4]);
      int fd = open(a, O_RDWR); if (fd < 0) { rc = -1; }
      else {
        struct stat st; fstat(fd, &st);
        if (st.st_size < off && ftruncate(fd, off) != 0) rc = -1;
        uint8_t *buf = malloc(len ? len : 1);
        for (long long i = 0; i < len; i++) buf[i] = (uint8_t)((((off + i) * 7) + seed) & 255);
        if (!rc && pwrite(fd, buf, len, off) != len) rc = -1;
        free(buf); close(fd);
      }
    } else die("unknown op");
    if (rc != 0) { fprintf(stderr, "OPS-FAIL: %s %s: %s\n", op, tok[1], strerror(errno)); }
  }
  fclose(f);
  fprintf(stderr, "OPS-DONE %d\n", n);
}

int main(void) {
  mount("proc", "/proc", "proc", 0, NULL);
  mount("sysfs", "/sys", "sysfs", 0, NULL);
  mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
  mkdir("/mnt", 0755);
  FILE *f = fopen("/job.txt", "r");
  if (!f) die("/job.txt");
  char line[4096];
  while (fgets(line, sizeof line, f)) {
    line[strcspn(line, "\n")] = 0;
    char *a = strtok(line, " ");
    if (!a) continue;
    char *p1 = strtok(NULL, " "), *p2 = strtok(NULL, " "), *p3 = strtok(NULL, " ");
    fprintf(stderr, "job: %s %s %s %s\n", a, p1 ? p1 : "", p2 ? p2 : "", p3 ? p3 : "");
    if (!strcmp(a, "LOADMOD")) load_module(p1);
    else if (!strcmp(a, "MOUNT")) {
      int ro = p2 && !strcmp(p2, "ro");   /* "ro" = 読み取り専用 (MS_RDONLY) */
      if (mount(p1, "/mnt", "f2fs", ro ? MS_RDONLY : 0, (p2 && strcmp(p2, "-") && !ro) ? p2 : "") != 0) die("mount f2fs");   /* "-" = オプションなし */
    } else if (!strcmp(a, "OPS")) run_ops(p1);
    else if (!strcmp(a, "DUMPTREE")) { dump_tree("/mnt", ""); fprintf(stderr, "TREE-END\n"); }
    else if (!strcmp(a, "COPY")) copy_tree("/tree", "/mnt");
    else if (!strcmp(a, "SPECIAL")) {
      char path[1024]; snprintf(path, sizeof path, "/mnt/%s", p2);
      mode_t m = !strcmp(p1, "p") ? S_IFIFO : !strcmp(p1, "s") ? S_IFSOCK : !strcmp(p1, "c") ? S_IFCHR : S_IFBLK;
      dev_t dv = !strcmp(p1, "c") ? makedev(1, 3) : !strcmp(p1, "b") ? makedev(8, 1) : 0;
      if (mknod(path, m | 0644, dv) != 0) die(path);
      set_mtime(path, atol(p3), 0);
    } else if (!strcmp(a, "W")) { char path[1024]; snprintf(path, sizeof path, "/mnt/%s", p1); write_piece(path, atol(p2), p3, 0, 0); }
    else if (!strcmp(a, "T")) { char path[1024]; snprintf(path, sizeof path, "/mnt/%s", p1); if (truncate(path, atol(p2)) != 0) die(path); }
    else if (!strcmp(a, "MT")) { char path[1024]; snprintf(path, sizeof path, "/mnt/%s", p1); set_mtime(path, atol(p2), 0); }
    else if (!strcmp(a, "APPENDFSYNC")) { char path[1024]; snprintf(path, sizeof path, "/mnt/%s", p1); write_piece(path, 0, p2, 1, 1); }
    else if (!strcmp(a, "NEWFSYNC")) { char path[1024]; snprintf(path, sizeof path, "/mnt/%s", p1); write_piece(path, 0, p2, 1, 0); }
    else if (!strcmp(a, "SYNC")) sync();
    else if (!strcmp(a, "UMOUNT")) { sync(); if (umount("/mnt") != 0) die("umount"); }
    else if (!strcmp(a, "POWEROFF")) { fprintf(stderr, "INIT-DONE (poweroff)\n"); fflush(stderr); syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_POWER_OFF, 0); for (;;) pause(); }
    else die("unknown job");
  }
  fprintf(stderr, "INIT-DONE (end of job)\n");
  fflush(stderr);
  syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_POWER_OFF, 0);
  for (;;) pause();
}
