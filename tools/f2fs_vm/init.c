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
      if (mount(p1, "/mnt", "f2fs", 0, (p2 && strcmp(p2, "-")) ? p2 : "") != 0) die("mount f2fs");   /* "-" = オプションなし */
    } else if (!strcmp(a, "COPY")) copy_tree("/tree", "/mnt");
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
