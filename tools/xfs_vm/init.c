// SPDX-License-Identifier: Apache-2.0
// 実カーネルに XFS のログを再生させ、結果を読み出すための VM 用 init (initramfs の /init。static にビルドする)。tools/xfs_vm/kreplay.py が使う。docs/xfs-write.md
// /job.txt: 1 行目 = カーネルモジュールのパス (xfs.ko)、2 行目 = POST (0/1)、3 行目以降 = ブロックデバイス (/dev/vda ...)。
// 各デバイスについて: xfs を rw でマウント (ここでカーネルがログを再生する) → ツリー全体を "K|<番号>|種別 サイズ sha256 パス" で出力 →
// POST=1 なら カーネルに追加の書き込み (mkdir / ファイル / シンボリックリンク + fsync) をさせる → アンマウント。結果は "K|<番号>|RESULT ok" / "RESULT mountfail <errno>" など。
#define _GNU_SOURCE
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/reboot.h>
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <unistd.h>

static int g_idx;
static void say(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void say(const char *fmt, ...) { va_list ap; va_start(ap, fmt); printf("K|%d|", g_idx); vprintf(fmt, ap); printf("\n"); va_end(ap); fflush(stdout); }
static void off(void) { fflush(stdout); sync(); syscall(SYS_reboot, LINUX_REBOOT_MAGIC1, LINUX_REBOOT_MAGIC2, LINUX_REBOOT_CMD_POWER_OFF, 0); for (;;) pause(); }

// ---- SHA-256
typedef struct { uint32_t h[8]; uint64_t n; uint8_t b[64]; size_t bl; } Sha;
static const uint32_t K[64] = {0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
#define ROR(x, n) (((x) >> (n)) | ((x) << (32 - (n))))
static void sha_blk(Sha *s, const uint8_t *p) {
  uint32_t w[64], a, b, c, d, e, f, g, h;
  for (int i = 0; i < 16; i++) w[i] = ((uint32_t)p[i*4] << 24) | ((uint32_t)p[i*4+1] << 16) | ((uint32_t)p[i*4+2] << 8) | p[i*4+3];
  for (int i = 16; i < 64; i++) { uint32_t s0 = ROR(w[i-15], 7) ^ ROR(w[i-15], 18) ^ (w[i-15] >> 3), s1 = ROR(w[i-2], 17) ^ ROR(w[i-2], 19) ^ (w[i-2] >> 10); w[i] = w[i-16] + s0 + w[i-7] + s1; }
  a = s->h[0]; b = s->h[1]; c = s->h[2]; d = s->h[3]; e = s->h[4]; f = s->h[5]; g = s->h[6]; h = s->h[7];
  for (int i = 0; i < 64; i++) {
    uint32_t t1 = h + (ROR(e, 6) ^ ROR(e, 11) ^ ROR(e, 25)) + ((e & f) ^ (~e & g)) + K[i] + w[i];
    uint32_t t2 = (ROR(a, 2) ^ ROR(a, 13) ^ ROR(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
    h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
  }
  s->h[0] += a; s->h[1] += b; s->h[2] += c; s->h[3] += d; s->h[4] += e; s->h[5] += f; s->h[6] += g; s->h[7] += h;
}
static void sha_init(Sha *s) { static const uint32_t i0[8] = {0x6a09e667,0xbb67ae85,0x3c6ef372,0xa54ff53a,0x510e527f,0x9b05688c,0x1f83d9ab,0x5be0cd19}; memcpy(s->h, i0, sizeof i0); s->n = 0; s->bl = 0; }
static void sha_upd(Sha *s, const uint8_t *p, size_t n) { s->n += n; while (n) { size_t k = 64 - s->bl; if (k > n) k = n; memcpy(s->b + s->bl, p, k); s->bl += k; p += k; n -= k; if (s->bl == 64) { sha_blk(s, s->b); s->bl = 0; } } }
static void sha_hex(Sha *s, char *out) {
  uint64_t bits = s->n * 8; uint8_t pad = 0x80; sha_upd(s, &pad, 1); pad = 0; while (s->bl != 56) sha_upd(s, &pad, 1);
  uint8_t L[8]; for (int i = 0; i < 8; i++) L[i] = (uint8_t)(bits >> (56 - 8 * i)); sha_upd(s, L, 8);
  for (int i = 0; i < 8; i++) sprintf(out + i * 8, "%08x", s->h[i]);
}

static int cmp(const void *a, const void *b) { return strcmp(*(char *const *)a, *(char *const *)b); }
static void walk(const char *base, const char *rel) {
  char dir[4096]; snprintf(dir, sizeof dir, "%s%s%s", base, *rel ? "/" : "", rel);
  DIR *d = opendir(dir);
  if (!d) { say("treefail opendir %s: %s", dir, strerror(errno)); return; }
  char **names = NULL; size_t n = 0, cap = 0; struct dirent *e;
  while ((e = readdir(d))) { if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue; if (n == cap) { cap = cap ? cap * 2 : 64; names = realloc(names, cap * sizeof *names); } names[n++] = strdup(e->d_name); }
  closedir(d);
  qsort(names, n, sizeof *names, cmp);
  for (size_t i = 0; i < n; i++) {
    char r[4096], p[8192]; snprintf(r, sizeof r, "%s%s%s", rel, *rel ? "/" : "", names[i]); snprintf(p, sizeof p, "%s/%s", base, r);
    struct stat st;
    if (lstat(p, &st) != 0) { say("treefail lstat %s: %s", r, strerror(errno)); free(names[i]); continue; }
    if (S_ISLNK(st.st_mode)) { char t[4096]; ssize_t k = readlink(p, t, sizeof t - 1); if (k < 0) k = 0; t[k] = 0; say("l 0 %s %s", t, r); }
    else if (S_ISDIR(st.st_mode)) { say("d 0 - %s", r); walk(base, r); }
    else if (S_ISREG(st.st_mode)) {
      int fd = open(p, O_RDONLY); Sha s; sha_init(&s); static uint8_t buf[1 << 16]; size_t tot = 0; ssize_t k;
      if (fd < 0) { say("u 0 - %s", r); free(names[i]); continue; }
      while ((k = read(fd, buf, sizeof buf)) > 0) { sha_upd(&s, buf, (size_t)k); tot += (size_t)k; }
      if (k < 0) say("treefail read %s: %s", r, strerror(errno));
      close(fd); char hx[65]; sha_hex(&s, hx); say("f %zu %s %s", tot, hx, r);
    }
    free(names[i]);
  }
  free(names);
}

static void post(void) {
  // カーネルが (再生後の) ボリュームに書く: 空き領域・inode のツリーを信用して割り当てる
  if (mkdir("/mnt/kpost", 0755) != 0 && errno != EEXIST) { say("postfail mkdir: %s", strerror(errno)); return; }
  static char buf[70000];
  for (int i = 0; i < 6; i++) {
    char p[64]; snprintf(p, sizeof p, "/mnt/kpost/k%d", i);
    int fd = open(p, O_WRONLY | O_CREAT | O_TRUNC, 0644);
    if (fd < 0) { say("postfail open %s: %s", p, strerror(errno)); return; }
    for (size_t j = 0; j < sizeof buf; j++) buf[j] = (char)((j * 7 + (size_t)i) & 255);
    size_t len = 5000 * (size_t)(i + 1) * 2; if (len > sizeof buf) len = sizeof buf;
    if (write(fd, buf, len) != (ssize_t)len) { say("postfail write %s: %s", p, strerror(errno)); close(fd); return; }
    if (fsync(fd) != 0) { say("postfail fsync: %s", strerror(errno)); close(fd); return; }
    close(fd);
  }
  if (symlink("k0", "/mnt/kpost/lnk") != 0 && errno != EEXIST) say("postfail symlink: %s", strerror(errno));
  char p[64]; for (int i = 0; i < 100; i++) { snprintf(p, sizeof p, "/mnt/kpost/e%03d", i); int fd = open(p, O_WRONLY | O_CREAT, 0644); if (fd >= 0) close(fd); }
  sync();
}

int main(void) {
  mount("proc", "/proc", "proc", 0, NULL); mount("sysfs", "/sys", "sysfs", 0, NULL); mount("devtmpfs", "/dev", "devtmpfs", 0, NULL);
  mkdir("/mnt", 0755);
  FILE *f = fopen("/job.txt", "r"); if (!f) { g_idx = -1; say("INIT-FAIL no job.txt"); off(); }
  char mod[256], line[256]; int dopost = 0;
  if (!fgets(mod, sizeof mod, f)) off();
  mod[strcspn(mod, "\n")] = 0;
  if (!fgets(line, sizeof line, f)) off();
  dopost = atoi(line);
  int fd = open(mod, O_RDONLY);
  if (fd < 0 || (syscall(SYS_finit_module, fd, "", 0) != 0 && errno != EEXIST)) { g_idx = -1; say("INIT-FAIL load xfs.ko: %s", strerror(errno)); off(); }
  close(fd);
  int idx = 0;
  while (fgets(line, sizeof line, f)) {
    line[strcspn(line, "\n")] = 0;
    if (!*line) continue;
    g_idx = idx++;
    if (mount(line, "/mnt", "xfs", 0, "") != 0) { say("RESULT mountfail %d", errno); continue; }
    walk("/mnt", "");
    if (dopost) post();
    sync();
    if (umount("/mnt") != 0) { say("RESULT umountfail %d", errno); continue; }
    say("RESULT ok");
  }
  g_idx = -1; say("INIT-DONE");
  off();
  return 0;
}
