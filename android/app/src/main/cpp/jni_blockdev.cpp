// SPDX-License-Identifier: Apache-2.0
// ディスクイメージ (QCOW2/VHD/VHDX/VMDK) / COW オーバーレイ / NBD の JNI。Kotlin 側は BlockDev.kt の BlockDevNative。
// 方針は jni_bridge.cpp と同じ: 薄く、例外を出さず、文字列は UTF-8 の byte[]。ハンドルは usbmem_device* (jlong)。
// 戻り値: ハンドル / <=0 の小さな負数 = -(usbmem::Errc)。ハンドルを閉じるのは NativeBridge.nativeClose。
#include <jni.h>

#include <cstring>
#include <string>

#include "usbmem/c_api_blockdev.h"

namespace {

std::string to_str(JNIEnv* env, jbyteArray a) {
  if (!a) return std::string();
  jsize n = env->GetArrayLength(a);
  std::string s(static_cast<size_t>(n), '\0');
  if (n) env->GetByteArrayRegion(a, 0, n, reinterpret_cast<jbyte*>(&s[0]));
  return s;
}

jbyteArray to_bytes(JNIEnv* env, const char* s) {
  size_t n = std::strlen(s);
  jbyteArray arr = env->NewByteArray(static_cast<jsize>(n));
  if (arr) env->SetByteArrayRegion(arr, 0, static_cast<jsize>(n), reinterpret_cast<const jbyte*>(s));
  return arr;
}

// 文字列 (usbmem_free で解放) を out[0] (Array<ByteArray?>) に入れる
void put_string(JNIEnv* env, jobjectArray out, char* s) {
  if (!s) return;
  if (out && env->GetArrayLength(out) > 0) {
    jbyteArray b = to_bytes(env, s);
    if (b) {
      env->SetObjectArrayElement(out, 0, b);
      env->DeleteLocalRef(b);
    }
  }
  usbmem_free(s);
}

struct ResolverCtx {
  JNIEnv* env;
  jobject resolver;
  jmethodID mid;
};

// Kotlin: fun interface ImageResolver { fun open(name: ByteArray, kind: Int, formatHint: ByteArray): Int }  (戻り値 = detach した fd、失敗は負)
int resolve_cb(void* user, const char* name, int kind, const char* hint, usbmem_device** out) {
  auto* c = static_cast<ResolverCtx*>(user);
  JNIEnv* env = c->env;
  jbyteArray jn = to_bytes(env, name);
  jbyteArray jh = to_bytes(env, hint);
  if (!jn || !jh) {
    if (env->ExceptionCheck()) env->ExceptionClear();
    return 10;   // NoMemory
  }
  jint fd = env->CallIntMethod(c->resolver, c->mid, jn, static_cast<jint>(kind), jh);
  env->DeleteLocalRef(jn);
  env->DeleteLocalRef(jh);
  if (env->ExceptionCheck()) {   // コールバックの例外は握りつぶして I/O エラー扱い (JNI の外へ出さない)
    env->ExceptionClear();
    return 2;
  }
  if (fd < 0) return 2;   // Io
  return usbmem_open_fd(fd, /*read_only=*/1, /*take_ownership=*/1, out);
}

}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL Java_org_usbmem_app_BlockDevNative_nativeImageOpen(JNIEnv* env, jclass, jlong fileHandle, jobject resolver, jobjectArray infoOut) {
  usbmem_device* out = nullptr;
  char* info = nullptr;
  int rc;
  if (resolver) {
    jclass cls = env->GetObjectClass(resolver);
    jmethodID mid = cls ? env->GetMethodID(cls, "open", "([BI[B)I") : nullptr;
    if (!mid) {
      if (env->ExceptionCheck()) env->ExceptionClear();
      return -1;   // InvalidArgument
    }
    ResolverCtx ctx{env, resolver, mid};
    rc = usbmem_image_open(reinterpret_cast<usbmem_device*>(fileHandle), resolve_cb, &ctx, &out, &info);
  } else {
    rc = usbmem_image_open(reinterpret_cast<usbmem_device*>(fileHandle), nullptr, nullptr, &out, &info);
  }
  put_string(env, infoOut, info);
  if (rc != 0) return -static_cast<jlong>(rc);
  return reinterpret_cast<jlong>(out);
}

JNIEXPORT jlong JNICALL Java_org_usbmem_app_BlockDevNative_nativeImageOpenPath(JNIEnv* env, jclass, jbyteArray path, jboolean allowUnsafe, jobjectArray infoOut) {
  usbmem_device* out = nullptr;
  char* info = nullptr;
  std::string p = to_str(env, path);
  int rc = usbmem_image_open_path(p.c_str(), allowUnsafe ? 1 : 0, &out, &info);
  put_string(env, infoOut, info);
  if (rc != 0) return -static_cast<jlong>(rc);
  return reinterpret_cast<jlong>(out);
}

JNIEXPORT jbyteArray JNICALL Java_org_usbmem_app_BlockDevNative_nativeImageDetect(JNIEnv* env, jclass, jlong fileHandle) {
  char buf[16] = {0};
  if (usbmem_image_detect(reinterpret_cast<usbmem_device*>(fileHandle), buf, sizeof buf) != 0) return nullptr;
  return to_bytes(env, buf);
}

JNIEXPORT jlong JNICALL Java_org_usbmem_app_BlockDevNative_nativeOverlayCreate(JNIEnv* env, jclass, jlong base, jbyteArray logPath, jint blockSize, jlong maxBytes) {
  usbmem_device* out = nullptr;
  std::string p = to_str(env, logPath);
  int rc = usbmem_overlay_create(reinterpret_cast<usbmem_device*>(base), logPath ? p.c_str() : nullptr, static_cast<unsigned>(blockSize),
                                 static_cast<unsigned long long>(maxBytes), &out);
  if (rc != 0) return -static_cast<jlong>(rc);
  return reinterpret_cast<jlong>(out);
}

JNIEXPORT jlong JNICALL Java_org_usbmem_app_BlockDevNative_nativeOverlayDirtyBytes(JNIEnv*, jclass, jlong h) {
  return static_cast<jlong>(usbmem_overlay_dirty_bytes(reinterpret_cast<usbmem_device*>(h)));
}

JNIEXPORT jint JNICALL Java_org_usbmem_app_BlockDevNative_nativeOverlayCommit(JNIEnv*, jclass, jlong h, jlong target, jboolean clearAfter) {
  return usbmem_overlay_commit(reinterpret_cast<usbmem_device*>(h), reinterpret_cast<usbmem_device*>(target), clearAfter ? 1 : 0);
}

JNIEXPORT jint JNICALL Java_org_usbmem_app_BlockDevNative_nativeOverlayDiscard(JNIEnv*, jclass, jlong h) {
  return usbmem_overlay_discard(reinterpret_cast<usbmem_device*>(h));
}

// NBD。ネットワーク越し (host 指定) には Manifest の INTERNET 権限が必要。UNIX ソケット (unixSocket 指定) は不要。
JNIEXPORT jlong JNICALL Java_org_usbmem_app_BlockDevNative_nativeNbdOpen(JNIEnv* env, jclass, jbyteArray host, jint port, jbyteArray unixSocket, jbyteArray exportName,
                                                                         jboolean readOnly, jint connectMs, jint ioMs, jobjectArray detailOut) {
  usbmem_device* out = nullptr;
  char* detail = nullptr;
  std::string h = to_str(env, host), u = to_str(env, unixSocket), e = to_str(env, exportName);
  int rc = usbmem_nbd_open(host ? h.c_str() : nullptr, port, unixSocket ? u.c_str() : nullptr, e.c_str(), readOnly ? 1 : 0,
                           static_cast<unsigned>(connectMs < 0 ? 0 : connectMs), static_cast<unsigned>(ioMs < 0 ? 0 : ioMs), &out, &detail);
  put_string(env, detailOut, detail);
  if (rc != 0) return -static_cast<jlong>(rc);
  return reinterpret_cast<jlong>(out);
}

JNIEXPORT jbyteArray JNICALL Java_org_usbmem_app_BlockDevNative_nativeNbdListExports(JNIEnv* env, jclass, jbyteArray host, jint port, jbyteArray unixSocket,
                                                                                    jintArray errorOut, jobjectArray detailOut) {
  char* json = nullptr;
  char* detail = nullptr;
  std::string h = to_str(env, host), u = to_str(env, unixSocket);
  int rc = usbmem_nbd_list_exports_json(host ? h.c_str() : nullptr, port, unixSocket ? u.c_str() : nullptr, &json, &detail);
  put_string(env, detailOut, detail);
  if (rc != 0 || !json) {
    jint e = rc;
    if (errorOut) env->SetIntArrayRegion(errorOut, 0, 1, &e);
    return nullptr;
  }
  jbyteArray arr = to_bytes(env, json);
  usbmem_free(json);
  return arr;
}

JNIEXPORT jbyteArray JNICALL Java_org_usbmem_app_BlockDevNative_nativeNbdInfo(JNIEnv* env, jclass, jlong h, jintArray errorOut) {
  char* json = nullptr;
  int rc = usbmem_nbd_info_json(reinterpret_cast<usbmem_device*>(h), &json);
  if (rc != 0 || !json) {
    jint e = rc;
    if (errorOut) env->SetIntArrayRegion(errorOut, 0, 1, &e);
    return nullptr;
  }
  jbyteArray arr = to_bytes(env, json);
  usbmem_free(json);
  return arr;
}

}  // extern "C"
