// SPDX-License-Identifier: Apache-2.0
// kfsd (docs/kfsd.md) の本体側の JNI: 方式の切り替え (nativeSetMode) と、RPC でのマウント。org.usbmem.app.kfsd.KfsdNative。
// kfsd の起こし方 (サービスの起動・socketpair の受け渡し) は Kotlin 側 (Kfsd.spawnChildFd): 接続済みソケットの fd (detach 済み。所有権はここへ) を返してもらう。
#include "jni_kfsd.h"

#include <jni.h>
#include <android/log.h>

#include <atomic>
#include <cerrno>
#include <ctime>

#include "usbmem/kfsd_client.h"

namespace {
std::atomic<int> g_mode{0};          // 0 = プロセス内 (従来)、1 = kfsd 経由 (RPC)
JavaVM* g_vm = nullptr;
jclass g_cls = nullptr;               // org.usbmem.app.kfsd.KfsdNative (グローバル参照)
jmethodID g_spawn_mid = nullptr;      // static int spawnChildFd()  (fd、または -errno)

int spawn_fd() {
  if (!g_vm || !g_cls || !g_spawn_mid) return -ENOSYS;
  JNIEnv* e = nullptr;
  bool attached = false;
  if (g_vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6) != JNI_OK) {
    if (g_vm->AttachCurrentThread(&e, nullptr) != JNI_OK) return -EIO;
    attached = true;
  }
  jint fd = e->CallStaticIntMethod(g_cls, g_spawn_mid);
  if (e->ExceptionCheck()) { e->ExceptionClear(); fd = -EIO; }
  if (attached) g_vm->DetachCurrentThread();
  return fd;
}
}  // namespace

bool usbmem_kfsd_rpc_enabled() { return g_mode.load() != 0; }

std::unique_ptr<usbmem::FsVolume> usbmem_kfsd_mount(const usbmem::BlockDevicePtr& dev, bool rw, int* err, bool allow_noload, bool allow_dirty) {
  if (dev->is_read_only()) rw = false;  // プロセス内のマウントと同じ (下のデバイスが読み取り専用なら、書き込み可を要求されても読み取り専用)
  int fd = spawn_fd();
  if (fd < 0) {
    int e = -fd;
    // kfsd の実行ファイル / プラグインが無い・サービスにつながらないは、ENOPKG に寄せる (UI が「プラグインが必要」を案内する)
    if (err) *err = (e == ENOENT || e == ENOPKG || e == ENOSYS || e == ECONNREFUSED || e == ENODEV) ? usbmem::kfsd::kErrNoKfsd : e;
    __android_log_print(ANDROID_LOG_INFO, "usbmem", "kfsd spawn failed: %d", e);
    return nullptr;
  }
  usbmem::kfsd::MountOptions o;
  o.rw = rw;
  o.allow_noload = allow_noload;
  o.allow_dirty = allow_dirty;
  // FAT の更新時刻は書いた端末のローカル時刻 (タイムゾーン情報なし)。本体が「UTC より何分西か」を送る (kfsd が設定する)
  time_t now = time(nullptr);
  struct tm lt;
  if (localtime_r(&now, &lt)) o.tz_minutes_west = static_cast<int>(-lt.tm_gmtoff / 60);
  // docs/kfsd.md 4 章: 先読み・まとめ書き・readdir への stat 同梱で、プロセス内の 50〜90% の速さになる
  o.dev_readahead_kb = 1024;
  o.dev_write_coalesce_kb = 1024;
  o.prefetch_stat = true;
  int e = 0;
  auto v = usbmem::kfsd::RpcVolume::mount_fd(fd, -1, dev, o, &e);   // fd の所有権は RpcVolume へ (失敗でも閉じる)。pid は本体の子ではないので渡さない (終了は EOF / kClose)
  if (!v) {
    if (err) *err = e > 0 ? e : EIO;
    __android_log_print(ANDROID_LOG_INFO, "usbmem", "kfsd mount refused: errno %d", e);
    return nullptr;
  }
  return std::unique_ptr<usbmem::FsVolume>(std::move(v));
}

extern "C" {

// 方式を切り替える (0 = プロセス内、1 = kfsd 経由)。KfsdNative.spawnChildFd の場所もここで登録する。
JNIEXPORT void JNICALL Java_org_usbmem_app_kfsd_KfsdNative_nativeSetMode(JNIEnv* env, jclass cls, jint mode) {
  if (!g_vm) env->GetJavaVM(&g_vm);
  if (!g_cls) {
    g_cls = static_cast<jclass>(env->NewGlobalRef(cls));
    g_spawn_mid = env->GetStaticMethodID(cls, "spawnChildFd", "()I");
  }
  g_mode.store(mode != 0 ? 1 : 0);
}

JNIEXPORT jint JNICALL Java_org_usbmem_app_kfsd_KfsdNative_nativeGetMode(JNIEnv*, jclass) { return g_mode.load(); }

}  // extern "C"
