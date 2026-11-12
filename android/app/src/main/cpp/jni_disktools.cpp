// SPDX-License-Identifier: Apache-2.0
// DiskTools (整合性検査 / mkfs) の JNI (対応する Kotlin は DiskTools.kt)。jni_partedit.cpp と同じ作法:
// 例外を出さず、文字列は UTF-8 の byte[] で受け渡し、失敗は 0 以外の戻り値 (-(usbmem::Errc)) か null + errorOut。
// ハンドル (mkfs セッション / ジョブ) はポインタをそのまま jlong にしたもの (>0 かつ 4095 より大きい。エラーは小さな負の値)。
#include <jni.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "usbmem/c_api_disktools.h"

namespace {

usbmem_mkfs* mk(jlong h) { return reinterpret_cast<usbmem_mkfs*>(h); }
usbmem_job* jb(jlong h) { return reinterpret_cast<usbmem_job*>(h); }
usbmem_device* dv(jlong h) { return reinterpret_cast<usbmem_device*>(h); }

struct Utf8 {
  std::string s;
  bool has = false;
  Utf8(JNIEnv* env, jbyteArray a) {
    if (!a) return;
    has = true;
    jsize n = env->GetArrayLength(a);
    s.resize(static_cast<size_t>(n));
    if (n) env->GetByteArrayRegion(a, 0, n, reinterpret_cast<jbyte*>(&s[0]));
    size_t z = s.find('\0');
    if (z != std::string::npos) s.resize(z);
  }
  const char* c() const { return has ? s.c_str() : nullptr; }
};

jbyteArray to_bytes(JNIEnv* env, const void* p, size_t n) {
  jbyteArray arr = env->NewByteArray(static_cast<jsize>(n));
  if (arr && n) env->SetByteArrayRegion(arr, 0, static_cast<jsize>(n), reinterpret_cast<const jbyte*>(p));
  return arr;
}

void set_err(JNIEnv* env, jintArray err, int rc) {
  if (err && env->GetArrayLength(err) >= 1) {
    jint e = rc;
    env->SetIntArrayRegion(err, 0, 1, &e);
  }
}

// 成功なら UTF-8 byte[]、失敗なら null と errorOut[0]
template <typename F>
jbyteArray json_call(JNIEnv* env, jintArray err, F f) {
  char* out = nullptr;
  int rc = f(&out);
  if (rc != 0 || !out) {
    set_err(env, err, rc ? rc : 12);
    if (out) usbmem_free(out);
    return nullptr;
  }
  jbyteArray arr = to_bytes(env, out, std::strlen(out));
  usbmem_free(out);
  return arr;
}

// 失敗でも結果 JSON が返るもの (キャンセル・適用失敗)。JSON があれば常に返し、errorOut[0] に戻り値を入れる
template <typename F>
jbyteArray result_call(JNIEnv* env, jintArray err, F f) {
  char* out = nullptr;
  int rc = f(&out);
  set_err(env, err, rc);
  if (!out) return nullptr;
  jbyteArray arr = to_bytes(env, out, std::strlen(out));
  usbmem_free(out);
  return arr;
}

jint neg(int rc) { return rc ? -rc : 0; }

}  // namespace

#define DT(name) Java_org_usbmem_app_DiskTools_##name

extern "C" {

// ---- ジョブ (進捗とキャンセル)
JNIEXPORT jlong JNICALL DT(nativeJobNew)(JNIEnv*, jclass) { return reinterpret_cast<jlong>(usbmem_job_new()); }
JNIEXPORT void JNICALL DT(nativeJobFree)(JNIEnv*, jclass, jlong j) { usbmem_job_free(jb(j)); }
JNIEXPORT void JNICALL DT(nativeJobCancel)(JNIEnv*, jclass, jlong j) { usbmem_job_cancel(jb(j)); }
// 戻り値: 段階名 (UTF-8)。progress[0] = done, progress[1] = total
JNIEXPORT jbyteArray JNICALL DT(nativeJobProgress)(JNIEnv* env, jclass, jlong j, jlongArray progress) {
  char ph[64] = {};
  unsigned long long d = 0, t = 0;
  usbmem_job_progress(jb(j), ph, sizeof ph, &d, &t);
  if (progress && env->GetArrayLength(progress) >= 2) {
    jlong v[2] = {static_cast<jlong>(d), static_cast<jlong>(t)};
    env->SetLongArrayRegion(progress, 0, 2, v);
  }
  return to_bytes(env, ph, std::strlen(ph));
}

// ---- 検査 (読み取り専用)
JNIEXPORT jbyteArray JNICALL DT(nativeCheck)(JNIEnv* env, jclass, jlong dev, jlong offset, jlong length, jbyteArray fsName, jint maxFindings,
                                             jlong memoryLimit, jlong job, jintArray err) {
  Utf8 fs(env, fsName);
  return result_call(env, err, [&](char** o) {
    return usbmem_fscheck_run(dv(dev), static_cast<unsigned long long>(offset), static_cast<unsigned long long>(length), fs.c(),
                              static_cast<unsigned>(maxFindings), static_cast<unsigned long long>(memoryLimit), jb(job), o);
  });
}
JNIEXPORT jboolean JNICALL DT(nativeCheckSupported)(JNIEnv* env, jclass, jbyteArray fsName) {
  Utf8 fs(env, fsName);
  return usbmem_fscheck_supported(fs.c()) ? JNI_TRUE : JNI_FALSE;
}

// ---- mkfs
// 戻り値: >4095 ハンドル / <=0 = -(Errc)
JNIEXPORT jlong JNICALL DT(nativeMkfsOpen)(JNIEnv* env, jclass, jlong dev, jbyteArray fsName, jlong offset, jlong length, jbyteArray label,
                                           jint clusterSize, jint sectorSize, jint inodeRatio, jint inodeSize, jboolean inUse, jlong seed,
                                           jlong timestamp) {
  Utf8 fs(env, fsName), lb(env, label);
  usbmem_mkfs* h = nullptr;
  int rc = usbmem_mkfs_open(dv(dev), fs.c(), static_cast<unsigned long long>(offset), static_cast<unsigned long long>(length), lb.c(),
                            static_cast<unsigned>(clusterSize), static_cast<unsigned>(sectorSize), static_cast<unsigned>(inodeRatio),
                            static_cast<unsigned>(inodeSize), inUse ? 1 : 0, static_cast<unsigned long long>(seed),
                            static_cast<unsigned long long>(timestamp), &h);
  if (rc != 0) return -static_cast<jlong>(rc);
  return reinterpret_cast<jlong>(h);
}
JNIEXPORT void JNICALL DT(nativeMkfsClose)(JNIEnv*, jclass, jlong h) { usbmem_mkfs_close(mk(h)); }
// "code\nmessage" (UTF-8)。h == 0 なら直近の open の失敗
JNIEXPORT jbyteArray JNICALL DT(nativeMkfsLastError)(JNIEnv* env, jclass, jlong h) {
  char c[64] = {}, m[512] = {};
  usbmem_mkfs_last_error(mk(h), c, sizeof c, m, sizeof m);
  std::string s = std::string(c) + "\n" + m;
  return to_bytes(env, s.data(), s.size());
}
JNIEXPORT jbyteArray JNICALL DT(nativeMkfsPlanJson)(JNIEnv* env, jclass, jlong h, jintArray err) {
  return json_call(env, err, [&](char** o) { return usbmem_mkfs_plan_json(mk(h), o); });
}
JNIEXPORT jbyteArray JNICALL DT(nativeMkfsMakeToken)(JNIEnv* env, jclass, jlong h, jbyteArray typed, jintArray err) {
  Utf8 t(env, typed);
  return json_call(env, err, [&](char** o) { return usbmem_mkfs_make_token(mk(h), t.c(), o); });
}
JNIEXPORT jbyteArray JNICALL DT(nativeMkfsApply)(JNIEnv* env, jclass, jlong h, jbyteArray token, jbyteArray backupPath, jlong timestamp, jboolean inUse,
                                                 jlong job, jintArray err) {
  Utf8 t(env, token), bp(env, backupPath);
  return result_call(env, err, [&](char** o) {
    return usbmem_mkfs_apply(mk(h), t.c(), bp.c(), static_cast<unsigned long long>(timestamp), inUse ? 1 : 0, jb(job), o);
  });
}
JNIEXPORT jlong JNICALL DT(nativeMkfsMinSize)(JNIEnv* env, jclass, jbyteArray fsName, jint sectorSize) {
  Utf8 fs(env, fsName);
  return static_cast<jlong>(usbmem_mkfs_min_size(fs.c(), static_cast<unsigned>(sectorSize)));
}
JNIEXPORT jbyteArray JNICALL DT(nativeMkfsLastBackup)(JNIEnv* env, jclass, jlong h, jintArray err) {
  unsigned char* p = nullptr;
  size_t n = 0;
  int rc = usbmem_mkfs_last_backup(mk(h), &p, &n);
  if (rc != 0 || !p) {
    set_err(env, err, rc ? rc : 12);
    if (p) usbmem_free(p);
    return nullptr;
  }
  jbyteArray arr = to_bytes(env, p, n);
  usbmem_free(p);
  return arr;
}
JNIEXPORT jbyteArray JNICALL DT(nativeMkfsBackupInfo)(JNIEnv* env, jclass, jbyteArray data, jintArray err) {
  if (!data) return nullptr;
  jsize n = env->GetArrayLength(data);
  std::vector<unsigned char> buf(static_cast<size_t>(n) + 1);
  if (n) env->GetByteArrayRegion(data, 0, n, reinterpret_cast<jbyte*>(buf.data()));
  return json_call(env, err, [&](char** o) { return usbmem_mkfs_backup_info_json(buf.data(), static_cast<size_t>(n), o); });
}
JNIEXPORT jint JNICALL DT(nativeMkfsRestore)(JNIEnv* env, jclass, jlong dev, jbyteArray data) {
  if (!data) return neg(1);
  jsize n = env->GetArrayLength(data);
  std::vector<unsigned char> buf(static_cast<size_t>(n) + 1);
  if (n) env->GetByteArrayRegion(data, 0, n, reinterpret_cast<jbyte*>(buf.data()));
  return neg(usbmem_mkfs_restore_backup(dv(dev), buf.data(), static_cast<size_t>(n)));
}

}  // extern "C"
