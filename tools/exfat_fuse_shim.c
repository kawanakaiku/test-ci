// exfat-fuse 1.4.0 (relan) は mount オプションに user=NAME を足すが、fusermount3 3.18 はそれを拒否する。
// libfuse が fusermount3 を起動する直前に "-o ...,user=NAME" から user= を取り除く LD_PRELOAD (テスト専用)。
#define _GNU_SOURCE
#include <dlfcn.h>
#include <spawn.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static char **fix(char *const argv[]) {
  int n = 0; while (argv[n]) n++;
  char **o = calloc((size_t)n + 1, sizeof *o);
  for (int i = 0; i < n; i++) {
    o[i] = argv[i];
    if (i > 0 && !strcmp(argv[i - 1], "-o")) {
      char *s = strdup(argv[i]), *w = s; char *save = NULL;
      for (char *t = strtok_r(strdup(argv[i]), ",", &save); t; t = strtok_r(NULL, ",", &save)) {
        if (!strncmp(t, "user=", 5) || !strcmp(t, "blkdev") || !strncmp(t, "blksize=", 8)) continue;
        if (w != s) *w++ = ',';
        size_t l = strlen(t); memcpy(w, t, l); w += l;
      }
      *w = 0; o[i] = s;
    }
  }
  return o;
}
static int is_fm(const char *p) { return p && strstr(p, "fusermount3"); }

int execv(const char *path, char *const argv[]) {
  int (*real)(const char *, char *const[]) = dlsym(RTLD_NEXT, "execv");
  return real(path, is_fm(path) ? fix(argv) : argv);
}
int execve(const char *path, char *const argv[], char *const envp[]) {
  int (*real)(const char *, char *const[], char *const[]) = dlsym(RTLD_NEXT, "execve");
  return real(path, is_fm(path) ? fix(argv) : argv, envp);
}
int execvp(const char *path, char *const argv[]) {
  int (*real)(const char *, char *const[]) = dlsym(RTLD_NEXT, "execvp");
  return real(path, is_fm(path) ? fix(argv) : argv);
}
int posix_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *at, char *const argv[], char *const envp[]) {
  int (*real)(pid_t *, const char *, const posix_spawn_file_actions_t *, const posix_spawnattr_t *, char *const[], char *const[]) = dlsym(RTLD_NEXT, "posix_spawn");
  return real(pid, path, fa, at, is_fm(path) ? fix(argv) : argv, envp);
}
int posix_spawnp(pid_t *pid, const char *path, const posix_spawn_file_actions_t *fa, const posix_spawnattr_t *at, char *const argv[], char *const envp[]) {
  int (*real)(pid_t *, const char *, const posix_spawn_file_actions_t *, const posix_spawnattr_t *, char *const[], char *const[]) = dlsym(RTLD_NEXT, "posix_spawnp");
  return real(pid, path, fa, at, is_fm(path) ? fix(argv) : argv, envp);
}
