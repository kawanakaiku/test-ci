// SPDX-License-Identifier: Apache-2.0
// kfsd (別プロセスの FS ドライバ。docs/kfsd.md) を起こす小さな JNI。org.usbmem.app.kfsd.KfsdSpawnNative。
// Java の ProcessBuilder は fd を子へ渡せない (0・1・2 以外を閉じる) ので、fork + exec で「本体とつながった socketpair の端」を子の fd 3 へ渡す
// (`kfsd --fd=3`)。個人版の :kfsd サービスと、別 APK のプラグインが同じものを使う。posix_spawn は API 28 以降なので使わない (minSdk 26)。
#include <jni.h>
#include <android/log.h>

#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cerrno>
#include <string>
#include <vector>

extern "C" {

// exe を `exe --fd=3 [extra...]` で起こす。sockFd は子の fd 3 へ複製する (呼び出し側の fd はそのまま。閉じるのは呼び出し側)。子の標準出力・標準エラーは
// パイプへ (読み側の fd を logOut[0] に返す。呼び出し側が読んで logcat へ流す)。戻り値 = pid、失敗は -errno。
JNIEXPORT jint JNICALL Java_org_usbmem_app_kfsd_KfsdSpawnNative_nativeSpawn(JNIEnv* env, jclass, jstring jexe, jint sockFd, jobjectArray jextra, jintArray logOut) {
  const char* e = env->GetStringUTFChars(jexe, nullptr);
  std::string exe = e ? e : "";
  if (e) env->ReleaseStringUTFChars(jexe, e);
  std::vector<std::string> args{exe, "--fd=3"};
  jsize n = jextra ? env->GetArrayLength(jextra) : 0;
  for (jsize i = 0; i < n; i++) {
    auto s = static_cast<jstring>(env->GetObjectArrayElement(jextra, i));
    const char* c = env->GetStringUTFChars(s, nullptr);
    args.emplace_back(c ? c : "");
    if (c) env->ReleaseStringUTFChars(s, c);
    env->DeleteLocalRef(s);
  }
  std::vector<char*> argv;
  for (auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  if (exe.empty() || ::access(exe.c_str(), X_OK) != 0) return -ENOENT;

  // 子に渡す fd を 10 以上へ移す (dup2 は同じ番号だと FD_CLOEXEC を落とさない。0〜3 とぶつかるのも避ける)
  int sock = ::fcntl(sockFd, F_DUPFD_CLOEXEC, 10);
  if (sock < 0) return -errno;
  int lp[2];
  if (::pipe2(lp, O_CLOEXEC) != 0) { int er = errno; ::close(sock); return -er; }
  int lw = ::fcntl(lp[1], F_DUPFD_CLOEXEC, 10);
  if (lw < 0) { int er = errno; ::close(sock); ::close(lp[0]); ::close(lp[1]); return -er; }
  int ep[2];   // exec の失敗を親へ伝える (成功すれば CLOEXEC で閉じて EOF)
  if (::pipe2(ep, O_CLOEXEC) != 0) { int er = errno; ::close(sock); ::close(lw); ::close(lp[0]); ::close(lp[1]); return -er; }

  pid_t p = ::fork();
  if (p == 0) {
    // 子: async-signal-safe な関数だけ
    if (::dup2(sock, 3) < 0 || ::dup2(lw, 1) < 0 || ::dup2(lw, 2) < 0) { int er = errno; ssize_t r = ::write(ep[1], &er, sizeof er); (void)r; _exit(127); }
    int nul = ::open("/dev/null", O_RDONLY);
    if (nul >= 0) ::dup2(nul, 0);
    ::execv(argv[0], argv.data());
    int er = errno;
    ssize_t r = ::write(ep[1], &er, sizeof er);
    (void)r;
    _exit(127);
  }
  int saved = errno;
  ::close(sock); ::close(lw); ::close(lp[1]); ::close(ep[1]);
  if (p < 0) { ::close(lp[0]); ::close(ep[0]); return -saved; }
  int er = 0;
  ssize_t r;
  do { r = ::read(ep[0], &er, sizeof er); } while (r < 0 && errno == EINTR);
  ::close(ep[0]);
  if (r == static_cast<ssize_t>(sizeof er)) { int st; ::waitpid(p, &st, 0); ::close(lp[0]); return -er; }
  if (logOut) { jint fd = lp[0]; env->SetIntArrayRegion(logOut, 0, 1, &fd); } else ::close(lp[0]);
  return p;
}

// pid の終了を待つ (ブロックする)。戻り値 = 終了コード (シグナル死は 128 + シグナル)、待てなければ -1。
JNIEXPORT jint JNICALL Java_org_usbmem_app_kfsd_KfsdSpawnNative_nativeWait(JNIEnv*, jclass, jint pid) {
  int st = 0;
  pid_t w;
  do { w = ::waitpid(pid, &st, 0); } while (w < 0 && errno == EINTR);
  if (w < 0) return -1;
  if (WIFEXITED(st)) return WEXITSTATUS(st);
  if (WIFSIGNALED(st)) return 128 + WTERMSIG(st);
  return -1;
}

}  // extern "C"
