// SPDX-License-Identifier: Apache-2.0
// 暗号ボリューム (BitLocker / LUKS) の JNI。資格情報は byte[] で受け取り、ネイティブ側のコピー (Bytes) は関数を出る時にゼロ化する。
// 渡された JVM 側の byte[] 自体は Kotlin 側 (CryptoVolume.kt) が呼び出し後に fill(0) する。
// 文字列は使わない (UTF-8 の byte[]。パスワードを String にすると JVM ヒープに残るため)。
#include <jni.h>

#include <cstring>
#include <vector>

#include "usbmem/c_api_transform.h"

namespace {
// byte[] の中身を secure に取り出す。呼び出し側の配列自体をゼロ化するのは Kotlin 側の責務だが、ここで作るコピーは必ずゼロ化する。
struct Bytes {
  std::vector<unsigned char> v;
  explicit Bytes(JNIEnv* env, jbyteArray a) {
    if (!a) return;
    jsize n = env->GetArrayLength(a);
    v.resize(static_cast<size_t>(n));
    if (n > 0) env->GetByteArrayRegion(a, 0, n, reinterpret_cast<jbyte*>(v.data()));
  }
  ~Bytes() {
    volatile unsigned char* p = v.data();
    for (size_t i = 0; i < v.size(); i++) p[i] = 0;
  }
  const unsigned char* data() const { return v.empty() ? nullptr : v.data(); }
  size_t size() const { return v.size(); }
};
jbyteArray to_array(JNIEnv* env, char* s) {
  if (!s) return nullptr;
  size_t n = std::strlen(s);
  jbyteArray arr = env->NewByteArray(static_cast<jsize>(n));
  if (arr) env->SetByteArrayRegion(arr, 0, static_cast<jsize>(n), reinterpret_cast<const jbyte*>(s));
  usbmem_free(s);
  return arr;
}
}  // namespace

extern "C" {

// 資格情報なしの判定。成功時 UTF-8 JSON (usbmem_transform_probe 参照)、失敗時 null (errorOut[0] = Errc)
JNIEXPORT jbyteArray JNICALL Java_org_usbmem_app_CryptoNative_nativeProbe(JNIEnv* env, jclass, jlong handle, jintArray errorOut) {
  char* json = nullptr;
  int rc = usbmem_transform_probe(reinterpret_cast<usbmem_device*>(handle), &json);
  if (rc != 0) {
    jint e = rc;
    if (errorOut && env->GetArrayLength(errorOut) >= 1) env->SetIntArrayRegion(errorOut, 0, 1, &e);
    return nullptr;
  }
  return to_array(env, json);
}

// 解錠。戻り値: >0 = 復号済みデバイスのハンドル (NativeBridge.nativeClose で閉じる。元の devHandle より先に閉じる) / <=0 = -(Errc)。
// infoOut[0] に VolumeInfo の JSON (取れた場合)。
JNIEXPORT jlong JNICALL Java_org_usbmem_app_CryptoNative_nativeOpen(JNIEnv* env, jclass, jlong handle, jint kind, jbyteArray password,
                                                                  jbyteArray recoveryKey, jbyteArray keyFile, jint flags,
                                                                  jobjectArray infoOut) {
  Bytes pw(env, password), rk(env, recoveryKey), kf(env, keyFile);
  usbmem_device* out = nullptr;
  char* info = nullptr;
  int rc = usbmem_transform_open(reinterpret_cast<usbmem_device*>(handle), kind, pw.data(), pw.size(), rk.data(), rk.size(), kf.data(),
                                 kf.size(), static_cast<unsigned>(flags), &out, &info);
  jbyteArray ia = to_array(env, info);
  if (infoOut && ia && env->GetArrayLength(infoOut) >= 1) env->SetObjectArrayElement(infoOut, 0, ia);
  if (rc != 0) return -static_cast<jlong>(rc);
  return reinterpret_cast<jlong>(out);
}

}  // extern "C"
