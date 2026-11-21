// SPDX-License-Identifier: GPL-2.0-or-later
// wimfuse (別プロセスの wimlib) の本体側の JNI。org.usbmem.app.wimfuse.WimFuseNative。docs/fuse-layer.md 10 章
// fd = 子 (libwimfuse.so) とつながった UNIX ソケット (Kotlin が起動・接続して detach したもの。所有権はここに移る)。
// dev = WIM ファイルを読む usbmem_device ハンドル (nativeOpenFd や nativeNestedOpen で得たもの。WIM を開いている間は閉じない)。
#include <jni.h>
#include <android/log.h>

#include <unistd.h>

#include <cerrno>
#include <memory>
#include <string>

#include "jni_vol.h"
#include "usbmem/c_api_internal.h"
#include "wimfuse/wim_volume.h"
#include "wimfuse/wimfuse_client.h"

namespace {
thread_local std::string t_msg;
JavaVM* g_vm = nullptr;
jclass g_cls = nullptr;       // org.usbmem.app.wimfuse.WimFuseNative (グローバル参照)
jmethodID g_spawn_mid = nullptr;  // static int spawnChildFd(long token)
}

extern "C" {

// WIM のヘッダとイメージ一覧 (JSON, UTF-8)。失敗なら null と err[0] = errno (エラー文は nativeLastMessage)。
JNIEXPORT jbyteArray JNICALL Java_org_usbmem_app_wimfuse_WimFuseNative_nativeInfo(JNIEnv* env, jclass, jint fd, jlong dev, jintArray errOut) {
  auto* d = reinterpret_cast<usbmem_device*>(dev);
  if (!d || !d->dev) { ::close(fd); return nullptr; }
  std::string out;
  int rc = usbmem::wimfuse::client_info(fd, d->dev, &out);
  if (rc != 0) {
    t_msg = out;
    jint e = rc < 0 ? -rc : EIO;
    if (errOut) env->SetIntArrayRegion(errOut, 0, 1, &e);
    return nullptr;
  }
  jbyteArray a = env->NewByteArray(static_cast<jsize>(out.size()));
  if (a) env->SetByteArrayRegion(a, 0, static_cast<jsize>(out.size()), reinterpret_cast<const jbyte*>(out.data()));
  return a;
}

// image (1 始まり) をマウントして Vol ハンドル (NtfsVolume と同じもの。閉じるのは nativeNtfsUnmount) を返す。失敗なら 0 と err[0] = errno。
JNIEXPORT jlong JNICALL Java_org_usbmem_app_wimfuse_WimFuseNative_nativeMount(JNIEnv* env, jclass, jint fd, jlong dev, jint image, jintArray errOut) {
  auto* d = reinterpret_cast<usbmem_device*>(dev);
  if (!d || !d->dev) { ::close(fd); return 0; }
  int err = 0;
  std::string msg;
  auto v = usbmem::wimfuse::client_mount(fd, d->dev, image, &err, &msg);
  if (!v) {
    t_msg = msg;
    __android_log_print(ANDROID_LOG_INFO, "usbmem", "wimfuse mount refused: errno %d (%s)", err, msg.c_str());
    jint e = err;
    if (errOut) env->SetIntArrayRegion(errOut, 0, 1, &e);
    return 0;
  }
  // 子に WIM を読ませるので、デバイスは Vol より長く生きる必要がある (RpcVolume のコールバックが shared_ptr で持つ)
  return reinterpret_cast<jlong>(new Vol(std::move(v)));
}

// WIM ファイル全体を、イメージごとのフォルダを並べた 1 つのボリュームとして開く (WimVolume)。子プロセスは、フォルダを最初に開いたときに Kotlin の
// WimFuseNative.spawnChildFd(token) を呼んで起こす (token は呼び出し側が登録した起こし方)。infoOut[0] にヘッダ・イメージ一覧の JSON を入れる。
// 戻り値: Vol ハンドル (nativeNtfsUnmount で閉じる)。失敗は 0 と err[0] = errno。dev は返したボリュームを閉じるまで開いておくこと。
JNIEXPORT jlong JNICALL Java_org_usbmem_app_wimfuse_WimFuseNative_nativeOpenArchive(JNIEnv* env, jclass cls, jlong dev, jlong token, jobjectArray infoOut, jintArray errOut) {
  auto* d = reinterpret_cast<usbmem_device*>(dev);
  if (!d || !d->dev) return 0;
  if (!g_vm) env->GetJavaVM(&g_vm);
  if (!g_cls) {
    g_cls = static_cast<jclass>(env->NewGlobalRef(cls));
    g_spawn_mid = env->GetStaticMethodID(cls, "spawnChildFd", "(J)I");
  }
  if (!g_spawn_mid) return 0;
  auto spawn = [token]() -> int {
    JNIEnv* e = nullptr;
    bool attached = false;
    if (g_vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6) != JNI_OK) {
      if (g_vm->AttachCurrentThread(&e, nullptr) != JNI_OK) return -EIO;
      attached = true;
    }
    jint fd = e->CallStaticIntMethod(g_cls, g_spawn_mid, token);
    if (e->ExceptionCheck()) { e->ExceptionClear(); fd = -EIO; }
    if (attached) g_vm->DetachCurrentThread();
    return fd;
  };
  int err = 0;
  std::string msg;
  auto v = usbmem::wimfuse::WimVolume::open(d->dev, spawn, &err, &msg);
  if (!v) {
    t_msg = msg;
    __android_log_print(ANDROID_LOG_INFO, "usbmem", "wimfuse archive open refused: errno %d (%s)", err, msg.c_str());
    jint e = err;
    if (errOut) env->SetIntArrayRegion(errOut, 0, 1, &e);
    return 0;
  }
  if (infoOut) {
    std::string j = v->info_json();
    jbyteArray a = env->NewByteArray(static_cast<jsize>(j.size()));
    if (a) { env->SetByteArrayRegion(a, 0, static_cast<jsize>(j.size()), reinterpret_cast<const jbyte*>(j.data())); env->SetObjectArrayElement(infoOut, 0, a); }
  }
  return reinterpret_cast<jlong>(new Vol(std::move(v)));
}

JNIEXPORT jbyteArray JNICALL Java_org_usbmem_app_wimfuse_WimFuseNative_nativeLastMessage(JNIEnv* env, jclass) {
  jbyteArray a = env->NewByteArray(static_cast<jsize>(t_msg.size()));
  if (a) env->SetByteArrayRegion(a, 0, static_cast<jsize>(t_msg.size()), reinterpret_cast<const jbyte*>(t_msg.data()));
  return a;
}

}  // extern "C"
