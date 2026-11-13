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

// アイドル自動スピンダウン: 1 = 止めた / 0 = 何もしない / <0 = -Errc
JNIEXPORT jint JNICALL Java_org_usbmem_app_NativeBridge_nativeUsbIdleStop(JNIEnv*, jclass, jlong handle, jlong idleMs) {
  if (idleMs < 0) return -22;
  return usbmem_usb_idle_stop(reinterpret_cast<usbmem_device*>(handle), static_cast<unsigned long long>(idleMs));
}

JNIEXPORT jboolean JNICALL Java_org_usbmem_app_NativeBridge_nativeUsbSpunDown(JNIEnv*, jclass, jlong handle) {
  return usbmem_usb_spun_down(reinterpret_cast<usbmem_device*>(handle)) ? JNI_TRUE : JNI_FALSE;
}

// 回転ディスクか、フラッシュ/SSD か。out = [kind, rpm, basis, hint, removable, probed]。戻り値 0 / <0 = -Errc。detail は診断の 1 行 (ASCII)
JNIEXPORT jint JNICALL Java_org_usbmem_app_NativeBridge_nativeUsbMediaInfo(JNIEnv* env, jclass, jlong handle, jintArray out, jobjectArray detailOut) {
  int v[6] = {0, 0, 0, 0, 0, 0};
  char detail[512];
  int rc = usbmem_usb_media_info(reinterpret_cast<usbmem_device*>(handle), v, detail, sizeof detail);
  if (rc != 0) return -rc;
  if (!out || env->GetArrayLength(out) < 6) return -22;
  jint j[6] = {v[0], v[1], v[2], v[3], v[4], v[5]};
  env->SetIntArrayRegion(out, 0, 6, j);
  if (detailOut && env->GetArrayLength(detailOut) >= 1) env->SetObjectArrayElement(detailOut, 0, env->NewStringUTF(detail));
  return 0;
}

// ディスクの回転を止める (start=0) / 再開 (start=1)。SCSI START STOP UNIT。0 / <0 = -Errc (未対応は -9)
JNIEXPORT jint JNICALL Java_org_usbmem_app_NativeBridge_nativeUsbStartStop(JNIEnv*, jclass, jlong handle, jboolean start) {
  int rc = usbmem_usb_start_stop(reinterpret_cast<usbmem_device*>(handle), start ? 1 : 0);
  return rc ? -rc : 0;
}

// 転送サイズの調整 (KiB 単位。0 = 変更しない)。0 / <0 = -Errc
JNIEXPORT jint JNICALL Java_org_usbmem_app_NativeBridge_nativeUsbTune(JNIEnv*, jclass, jlong handle, jint chunkKiB, jint maxTransferKiB) {
  if (chunkKiB < 0 || maxTransferKiB < 0 || chunkKiB > 16384 || maxTransferKiB > 16384) return -22;
  int rc = usbmem_usb_tune(reinterpret_cast<usbmem_device*>(handle), static_cast<size_t>(chunkKiB) * 1024, static_cast<size_t>(maxTransferKiB) * 1024);
  return rc ? -rc : 0;
}

// 読み取り専用の速度計測。戻り値: 経過マイクロ秒 (>0) / <=0 = -Errc
JNIEXPORT jlong JNICALL Java_org_usbmem_app_NativeBridge_nativeBenchRead(JNIEnv*, jclass, jlong handle, jlong offset, jlong totalBytes, jint bufKiB) {
  if (offset < 0 || totalBytes <= 0 || bufKiB <= 0) return -22;
  long long us = 0;
  int rc = usbmem_bench_read(reinterpret_cast<usbmem_device*>(handle), static_cast<unsigned long long>(offset),
                             static_cast<unsigned long long>(totalBytes), static_cast<size_t>(bufKiB) * 1024, &us, nullptr);
  return rc ? -static_cast<jlong>(rc) : static_cast<jlong>(us);
}

// 同上 + 読んだデータのハッシュ (outHash[0])。転送サイズを変えても同じデータが読めるかの確認用
JNIEXPORT jlong JNICALL Java_org_usbmem_app_NativeBridge_nativeBenchReadHash(JNIEnv* env, jclass, jlong handle, jlong offset, jlong totalBytes, jint bufKiB, jlongArray outHash) {
  if (offset < 0 || totalBytes <= 0 || bufKiB <= 0 || !outHash || env->GetArrayLength(outHash) < 1) return -22;
  long long us = 0; unsigned long long h = 0;
  int rc = usbmem_bench_read(reinterpret_cast<usbmem_device*>(handle), static_cast<unsigned long long>(offset),
                             static_cast<unsigned long long>(totalBytes), static_cast<size_t>(bufKiB) * 1024, &us, &h);
  if (rc) return -static_cast<jlong>(rc);
  jlong hv = static_cast<jlong>(h);
  env->SetLongArrayRegion(outHash, 0, 1, &hv);
  return static_cast<jlong>(us);
}

JNIEXPORT jstring JNICALL Java_org_usbmem_app_NativeBridge_nativeUsbLastOpenTrace(JNIEnv* env, jclass) {
  char buf[3400];
  usbmem_usb_last_open_trace(buf, sizeof buf);
  return env->NewStringUTF(buf);   // 16 進オペコード・errno 名のみの ASCII
}

JNIEXPORT jstring JNICALL Java_org_usbmem_app_NativeBridge_nativeUsbOpStats(JNIEnv* env, jclass, jlong handle) {
  char buf[512];
  usbmem_usb_op_stats(reinterpret_cast<usbmem_device*>(handle), buf, sizeof buf);
  return env->NewStringUTF(buf);   // 16 進オペコードと回数のみの ASCII
}

// 開いているデバイスの [offset, offset+length) を読み取り専用の別デバイスとして切り出す (暗号ボリュームの解錠 CryptoVolume.open に渡すパーティションの切り出し用)。
// 親のハンドルは消費しない。戻り値: >0 ハンドル (nativeClose で閉じる。親より先に) / <=0 = -(usbmem::Errc)
JNIEXPORT jlong JNICALL Java_org_usbmem_app_NativeBridge_nativeDeviceSubRange(JNIEnv*, jclass, jlong handle, jlong offset, jlong length) {
  if (!handle || offset < 0 || length <= 0) return -22;
  usbmem_device* out = nullptr;
  int rc = usbmem_device_sub_range(reinterpret_cast<usbmem_device*>(handle), static_cast<unsigned long long>(offset),
                                   static_cast<unsigned long long>(length), /*read_only=*/1, &out);
  if (rc != 0) return -static_cast<jlong>(rc);
  return reinterpret_cast<jlong>(out);
}

JNIEXPORT void JNICALL Java_org_usbmem_app_NativeBridge_nativeClose(JNIEnv*, jclass, jlong handle) {
  usbmem_close(reinterpret_cast<usbmem_device*>(handle));
}

JNIEXPORT jstring JNICALL Java_org_usbmem_app_NativeBridge_nativeErrcName(JNIEnv* env, jclass, jint code) {
  return env->NewStringUTF(usbmem_errc_name(code));  // 固定の ASCII 文字列
}

}  // extern "C"
