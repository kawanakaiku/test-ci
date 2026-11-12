// SPDX-License-Identifier: Apache-2.0
// データ救出 (ddrescue 方式) とバックアップ / 復元 (Partclone 方式) の JNI。Kotlin 側は RecoveryBackup.kt。
// 長い処理 (nativeRescueRun / nativeBackupRun / nativeRestoreApply) は呼び出したスレッドをふさぐので、必ず Dispatchers.IO などの別スレッドから呼ぶ。
// 取り消し・進捗は別スレッドから nativeJobCancel / nativeJobProgress で行う。結果は UTF-8 JSON の byte[] (c_api_recovery.h)。
#include <jni.h>

#include <cstring>
#include <string>

#include "usbmem/c_api_recovery.h"

namespace {
struct Utf {
  JNIEnv* env;
  jstring s;
  const char* p = nullptr;
  Utf(JNIEnv* e, jstring str) : env(e), s(str) { if (s) p = env->GetStringUTFChars(s, nullptr); }
  ~Utf() { if (p) env->ReleaseStringUTFChars(s, p); }
  const char* c() const { return p; }
};
jbyteArray to_array(JNIEnv* env, char* s) {
  if (!s) return nullptr;
  size_t n = std::strlen(s);
  jbyteArray arr = env->NewByteArray(static_cast<jsize>(n));
  if (arr) env->SetByteArrayRegion(arr, 0, static_cast<jsize>(n), reinterpret_cast<const jbyte*>(s));
  usbmem_free(s);
  return arr;
}
void put_out(JNIEnv* env, jobjectArray out, char* json) {
  jbyteArray a = to_array(env, json);
  if (out && a && env->GetArrayLength(out) >= 1) env->SetObjectArrayElement(out, 0, a);
}
usbmem_device* dev(jlong h) { return reinterpret_cast<usbmem_device*>(h); }
}  // namespace

extern "C" {

JNIEXPORT jlong JNICALL Java_org_usbmem_app_RecoveryNative_nativeJobNew(JNIEnv*, jclass) { return reinterpret_cast<jlong>(usbmem_job_new()); }
JNIEXPORT void JNICALL Java_org_usbmem_app_RecoveryNative_nativeJobCancel(JNIEnv*, jclass, jlong j) { usbmem_job_cancel(reinterpret_cast<usbmem_job*>(j)); }
JNIEXPORT void JNICALL Java_org_usbmem_app_RecoveryNative_nativeJobPause(JNIEnv*, jclass, jlong j, jboolean p) { usbmem_job_pause(reinterpret_cast<usbmem_job*>(j), p ? 1 : 0); }
JNIEXPORT void JNICALL Java_org_usbmem_app_RecoveryNative_nativeJobFree(JNIEnv*, jclass, jlong j) { usbmem_job_free(reinterpret_cast<usbmem_job*>(j)); }
JNIEXPORT jbyteArray JNICALL Java_org_usbmem_app_RecoveryNative_nativeJobProgress(JNIEnv* env, jclass, jlong j) {
  char* json = nullptr;
  if (usbmem_job_progress_json(reinterpret_cast<usbmem_job*>(j), &json) != 0) return nullptr;
  return to_array(env, json);
}

// opts: usbmem_rescue_options と同じ順の long 配列 (長さ 17)。戻り値 0 = 完了 / それ以外は Errc (結果の JSON は resultOut[0])
JNIEXPORT jint JNICALL Java_org_usbmem_app_RecoveryNative_nativeRescueRun(JNIEnv* env, jclass, jlong in, jlong out, jstring mapPath, jlongArray opts, jlong job,
                                                                         jobjectArray resultOut) {
  usbmem_rescue_options o;
  std::memset(&o, 0, sizeof o);
  if (opts && env->GetArrayLength(opts) >= 17) {
    jlong v[17];
    env->GetLongArrayRegion(opts, 0, 17, v);
    o.start = static_cast<unsigned long long>(v[0]); o.length = static_cast<unsigned long long>(v[1]); o.out_offset = static_cast<unsigned long long>(v[2]);
    o.sector_size = static_cast<unsigned>(v[3]); o.cluster_sectors = static_cast<unsigned>(v[4]); o.retries = static_cast<unsigned>(v[5]);
    o.no_trim = static_cast<int>(v[6]); o.no_scrape = static_cast<int>(v[7]); o.no_skip = static_cast<int>(v[8]);
    o.read_timeout_ms = static_cast<unsigned>(v[9]);
    o.max_consecutive_errors = static_cast<unsigned>(v[10]); o.max_errors = static_cast<unsigned long long>(v[11]);
    o.error_window_ms = static_cast<unsigned>(v[12]); o.max_errors_in_window = static_cast<unsigned>(v[13]);
    o.no_progress_timeout_ms = static_cast<unsigned long long>(v[14]); o.pause_between_reads_ms = static_cast<unsigned>(v[15]);
  }
  Utf mp(env, mapPath);
  char* json = nullptr;
  int rc = usbmem_rescue_run(dev(in), dev(out), mp.c(), &o, reinterpret_cast<usbmem_job*>(job), &json);
  put_out(env, resultOut, json);
  return rc;
}

JNIEXPORT jint JNICALL Java_org_usbmem_app_RecoveryNative_nativeBackupRun(JNIEnv* env, jclass, jlong src, jstring imagePath, jboolean lz4, jboolean full, jlong job,
                                                                         jobjectArray resultOut) {
  Utf p(env, imagePath);
  char* json = nullptr;
  int rc = usbmem_backup_run(dev(src), p.c(), lz4 ? 1 : 0, full ? 1 : 0, reinterpret_cast<usbmem_job*>(job), &json);
  put_out(env, resultOut, json);
  return rc;
}

JNIEXPORT jint JNICALL Java_org_usbmem_app_RecoveryNative_nativeInspect(JNIEnv* env, jclass, jstring imagePath, jboolean verify, jobjectArray resultOut) {
  Utf p(env, imagePath);
  char* json = nullptr;
  int rc = usbmem_backup_inspect_json(p.c(), verify ? 1 : 0, &json);
  put_out(env, resultOut, json);
  return rc;
}

// 戻り値 0 = 計画ができた (ok でなくても planOut[0] と JSON は返る)。planOut[0] は nativePlanFree で解放する。
JNIEXPORT jint JNICALL Java_org_usbmem_app_RecoveryNative_nativeRestorePlan(JNIEnv* env, jclass, jstring imagePath, jlong target, jboolean inUse, jboolean verifyAll,
                                                                           jlongArray planOut, jobjectArray jsonOut) {
  Utf p(env, imagePath);
  usbmem_restore_plan* plan = nullptr;
  char* json = nullptr;
  int rc = usbmem_restore_plan_new(p.c(), dev(target), inUse ? 1 : 0, verifyAll ? 1 : 0, &plan, &json);
  put_out(env, jsonOut, json);
  jlong h = reinterpret_cast<jlong>(plan);
  if (planOut && env->GetArrayLength(planOut) >= 1) env->SetLongArrayRegion(planOut, 0, 1, &h);
  return rc;
}

JNIEXPORT jint JNICALL Java_org_usbmem_app_RecoveryNative_nativeRestoreToken(JNIEnv* env, jclass, jlong plan, jstring typed, jobjectArray tokenOut) {
  Utf t(env, typed);
  char* tok = nullptr;
  int rc = usbmem_restore_token(reinterpret_cast<usbmem_restore_plan*>(plan), t.c(), &tok);
  put_out(env, tokenOut, tok);
  return rc;
}

JNIEXPORT jint JNICALL Java_org_usbmem_app_RecoveryNative_nativeRestoreApply(JNIEnv* env, jclass, jlong plan, jstring imagePath, jlong target, jstring token, jboolean inUse,
                                                                            jlong job, jobjectArray resultOut) {
  Utf p(env, imagePath), t(env, token);
  char* json = nullptr;
  int rc = usbmem_restore_apply(reinterpret_cast<usbmem_restore_plan*>(plan), p.c(), dev(target), t.c(), inUse ? 1 : 0, reinterpret_cast<usbmem_job*>(job), &json);
  put_out(env, resultOut, json);
  return rc;
}

JNIEXPORT void JNICALL Java_org_usbmem_app_RecoveryNative_nativePlanFree(JNIEnv*, jclass, jlong plan) { usbmem_restore_plan_free(reinterpret_cast<usbmem_restore_plan*>(plan)); }

}  // extern "C"
