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

// USB Mass Storage を開く。戻り値: >0 ハンドル / <=0 = -(usbmem::Errc)。fd の所有権は Kotlin (UsbDeviceConnection) に残る。
JNIEXPORT jlong JNICALL Java_org_usbmem_app_NativeBridge_nativeOpenUsbMsc(JNIEnv*, jclass, jint fd, jint epIn, jint epOut,
                                                                        jint iface, jint lun, jboolean readOnly) {
  usbmem_device* dev = nullptr;
  int rc = usbmem_open_usb_msc(fd, epIn, epOut, iface, lun, readOnly ? 1 : 0, &dev);
  if (rc != 0) return -static_cast<jlong>(rc);
  return reinterpret_cast<jlong>(dev);
}

// 直近の SCSI センス [key, asc, ascq] (診断表示用)
JNIEXPORT void JNICALL Java_org_usbmem_app_NativeBridge_nativeUsbLastSense(JNIEnv* env, jclass, jlong handle, jintArray out) {
  int k = 0, a = 0, q = 0;
  usbmem_usb_last_sense(reinterpret_cast<usbmem_device*>(handle), &k, &a, &q);
  jint v[3] = {k, a, q};
  if (out && env->GetArrayLength(out) >= 3) env->SetIntArrayRegion(out, 0, 3, v);
}

JNIEXPORT void JNICALL Java_org_usbmem_app_NativeBridge_nativeClose(JNIEnv*, jclass, jlong handle) {
  usbmem_close(reinterpret_cast<usbmem_device*>(handle));
}

JNIEXPORT jstring JNICALL Java_org_usbmem_app_NativeBridge_nativeErrcName(JNIEnv* env, jclass, jint code) {
  return env->NewStringUTF(usbmem_errc_name(code));  // 固定の ASCII 文字列
}

}  // extern "C"
