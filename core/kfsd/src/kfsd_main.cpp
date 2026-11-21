// SPDX-License-Identifier: GPL-2.0-or-later
// kfsd: カーネル由来 (GPL) のファイルシステム・ドライバを載せた別プロセスの実行ファイル。本体 (Apache-2.0) とは kfsd_proto.h のメッセージだけで通信する。
// 1 つのボリュームにつき 1 プロセス (使い捨て)。ブロックデバイスは持たず、本体へ callback で読み書きを頼む。docs/kfsd.md
//   kfsd --fd=N [--mem-limit-mb=M] [--no-pdeathsig]       N: 本体との socketpair の端
//   kfsd --version
// 終了コード: 0 正常 (kClose) / 1 本体が消えた / 2 プロトコル違反 / 3 マウント失敗 / 64 引数の誤り
#include <dirent.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include "kfsd_mount.h"
#include "usbmem/kfsd_server.h"

namespace {

// fd 以外の継承された fd を閉じる (本体が持つ USB の fd などが漏れていても、kfsd からは見えないように)。
void close_other_fds(int keep) {
  DIR* d = ::opendir("/proc/self/fd");
  if (!d) {
    for (int i = 3; i < 1024; i++) if (i != keep) ::close(i);
    return;
  }
  int dfd = ::dirfd(d);
  while (dirent* e = ::readdir(d)) {
    char* end = nullptr;
    long n = std::strtol(e->d_name, &end, 10);
    if (end == e->d_name || *end || n < 3 || n == keep || n == dfd) continue;
    ::close(static_cast<int>(n));
  }
  ::closedir(d);
}

}  // namespace

int main(int argc, char** argv) {
  int fd = -1;
  long mem_mb = 0;
  bool pdeathsig = true;
  for (int i = 1; i < argc; i++) {
    std::string a = argv[i];
    if (a.rfind("--fd=", 0) == 0) fd = std::atoi(a.c_str() + 5);
    else if (a.rfind("--mem-limit-mb=", 0) == 0) mem_mb = std::atol(a.c_str() + 15);
    else if (a == "--no-pdeathsig") pdeathsig = false;
    else if (a == "--version") { std::puts("usbmem-kfsd 1 (protocol 1)"); return 0; }
    else { std::fprintf(stderr, "kfsd: unknown argument: %s\n", a.c_str()); return 64; }
  }
  if (fd < 3) { std::fprintf(stderr, "usage: kfsd --fd=N (N >= 3; the socket to the app)\n"); return 64; }
  close_other_fds(fd);
  // 本体が死んだら自分も終わる。通常はソケットの EOF で足りる。これは、ドライバの中 (ソケットを読んでいない間) で止まっているときの保険。
  // 注意: PDEATHSIG は「親プロセスを作ったスレッド」の終了でも発火する。起こす側は終わらないスレッドから起こすこと (docs/kfsd.md)。
#ifdef __linux__
  if (pdeathsig) {
    pid_t pp = ::getppid();
    ::prctl(PR_SET_PDEATHSIG, SIGKILL);
    if (::getppid() != pp) return 1;   // 設定の前に親が死んだ
    ::prctl(PR_SET_DUMPABLE, 0);        // USB 上の信頼できないデータを扱うので、コアダンプ・ptrace を避ける
  }
  ::prctl(PR_SET_NO_NEW_PRIVS, 1, 0, 0, 0);
#else
  (void)pdeathsig;
#endif
  ::signal(SIGPIPE, SIG_IGN);
  struct rlimit rl;
  rl.rlim_cur = rl.rlim_max = 0;
  ::setrlimit(RLIMIT_CORE, &rl);
  if (mem_mb > 0) {
    rl.rlim_cur = rl.rlim_max = static_cast<rlim_t>(mem_mb) << 20;
    if (::setrlimit(RLIMIT_AS, &rl) != 0) std::fprintf(stderr, "kfsd: setrlimit(AS) failed: %s\n", std::strerror(errno));
  }
  ::umask(077);
  return usbmem::kfsd::serve(fd, usbmem::kfsd::mount_fn());
}
