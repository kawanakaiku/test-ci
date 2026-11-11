// SPDX-License-Identifier: Apache-2.0
// JNI は薄く保つ: 例外を出さず、文字列は UTF-8 の byte[] で受け渡す
// (NewStringUTF は modified UTF-8 前提で、絵文字などの 4 バイト文字を安全に扱えないため)。
#include <jni.h>

#include <cstring>

#include "usbmem/c_api.h"

extern "C" {

// 戻り値: >0 = ハンドル, <=0 = -(usbmem::Errc)
JNIEXPORT jlong JNICALL Java_org_usbmem_app_NativeBridge_nativeOpenFd(JNIEnv*, jclass, jint fd,
                                                                    jboolean readOnly) {
  usbmem_device* dev = nullptr;
  int rc = usbmem_open_fd(fd, readOnly ? 1 : 0, /*take_ownership=*/1, &dev);
  if (rc != 0) return -static_cast<jlong>(rc);
  return reinterpret_cast<jlong>(dev);
}

// 成功時 UTF-8 JSON, 失敗時 null (errorOut[0] にコード)
JNIEXPORT jbyteArray JNICALL Java_org_usbmem_app_NativeBridge_nativeListPartitions(JNIEnv* env, jclass,
                                                                                 jlong handle,
                                                                                 jintArray errorOut) {
  char* json = nullptr;
  int rc = usbmem_list_partitions_json(reinterpret_cast<usbmem_device*>(handle), &json);
  if (rc != 0 || !json) {
    jint e = rc;
    if (errorOut) env->SetIntArrayRegion(errorOut, 0, 1, &e);
    return nullptr;
  }
  size_t n = std::strlen(json);
  jbyteArray arr = env->NewByteArray(static_cast<jsize>(n));
  if (arr) env->SetByteArrayRegion(arr, 0, static_cast<jsize>(n), reinterpret_cast<const jbyte*>(json));
  usbmem_free(json);
  return arr;
}

JNIEXPORT void JNICALL Java_org_usbmem_app_NativeBridge_nativeClose(JNIEnv*, jclass, jlong handle) {
  usbmem_close(reinterpret_cast<usbmem_device*>(handle));
}

JNIEXPORT jstring JNICALL Java_org_usbmem_app_NativeBridge_nativeErrcName(JNIEnv* env, jclass, jint code) {
  return env->NewStringUTF(usbmem_errc_name(code));  // 固定の ASCII 文字列
}

}  // extern "C"
