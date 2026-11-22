// SPDX-License-Identifier: Apache-2.0
// パーティション探索 / 失われた表の復元案 / ブートセクタの検査と修復の JNI (対応する Kotlin は PartitionScan.kt)。jni_partedit.cpp と同じ作法:
// 例外を出さず、文字列は UTF-8 の byte[] で受け渡し、失敗は 0 以外の戻り値 (-(usbmem::Errc)) か null + errorOut。
// ハンドルはポインタをそのまま jlong にしたもの (>0 かつ 4095 より大きい。エラーは小さな負の値)。
#include <jni.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "usbmem/c_api_partscan.h"

namespace {

usbmem_pjob* jb(jlong h) { return reinterpret_cast<usbmem_pjob*>(h); }
usbmem_partscan* ps(jlong h) { return reinterpret_cast<usbmem_partscan*>(h); }
usbmem_bootfix* bf(jlong h) { return reinterpret_cast<usbmem_bootfix*>(h); }

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

void set_err(JNIEnv* env, jintArray errorOut, int rc) {
  if (errorOut && env->GetArrayLength(errorOut) >= 1) {
    jint e = rc ? rc : 12;
    env->SetIntArrayRegion(errorOut, 0, 1, &e);
  }
}

// 文字列を返す C API の共通ラッパ。cancelled のような「結果は有るが戻り値が 0 でない」呼び出しは keep_on_error = true で結果を返し、errorOut に戻り値を入れる
template <typename F>
jbyteArray json_call(JNIEnv* env, jintArray errorOut, F f, bool keep_on_error = false) {
  char* out = nullptr;
  int rc = f(&out);
  if (errorOut && env->GetArrayLength(errorOut) >= 1) {
    jint e = rc;
    env->SetIntArrayRegion(errorOut, 0, 1, &e);
  }
  if ((rc != 0 && !keep_on_error) || !out) {
    set_err(env, errorOut, rc);
    if (out) usbmem_free(out);
    return nullptr;
  }
  jbyteArray arr = to_bytes(env, out, std::strlen(out));
  usbmem_free(out);
  return arr;
}

jint neg(int rc) { return rc ? -rc : 0; }

}  // namespace

#define PS(name) Java_org_usbmem_app_PartitionScan_##name

extern "C" {

// ---- 進捗とキャンセル (別スレッドから呼んでよい)
JNIEXPORT jlong JNICALL PS(nativeJobNew)(JNIEnv*, jclass) { return reinterpret_cast<jlong>(usbmem_pjob_new()); }
JNIEXPORT void JNICALL PS(nativeJobFree)(JNIEnv*, jclass, jlong j) { usbmem_pjob_free(jb(j)); }
JNIEXPORT void JNICALL PS(nativeJobCancel)(JNIEnv*, jclass, jlong j) { usbmem_pjob_cancel(jb(j)); }
// "phase" を byte[] で返し、done / total は out[0] / out[1]
JNIEXPORT jbyteArray JNICALL PS(nativeJobProgress)(JNIEnv* env, jclass, jlong j, jlongArray out) {
  char ph[32] = {};
  unsigned long long d = 0, t = 0;
  usbmem_pjob_progress(jb(j), ph, sizeof ph, &d, &t);
  if (out && env->GetArrayLength(out) >= 2) {
    jlong v[2] = {static_cast<jlong>(d), static_cast<jlong>(t)};
    env->SetLongArrayRegion(out, 0, 2, v);
  }
  return to_bytes(env, ph, std::strlen(ph));
}

// ---- 探索。戻り値: >4095 ハンドル / <=0 = -(Errc)。キャンセルされても途中までの結果のハンドルを返し、outRc[0] = Errc::Cancelled
JNIEXPORT jlong JNICALL PS(nativeScanRun)(JNIEnv* env, jclass, jlong dev, jint sectorSize, jint level, jint minConfidence, jlong job, jintArray outRc) {
  usbmem_partscan* h = nullptr;
  int rc = usbmem_partscan_run(reinterpret_cast<usbmem_device*>(dev), static_cast<unsigned>(sectorSize), level, minConfidence, jb(job), &h);
  if (outRc && env->GetArrayLength(outRc) >= 1) {
    jint e = rc;
    env->SetIntArrayRegion(outRc, 0, 1, &e);
  }
  if (!h) return -static_cast<jlong>(rc ? rc : 12);
  return reinterpret_cast<jlong>(h);
}
JNIEXPORT void JNICALL PS(nativeScanClose)(JNIEnv*, jclass, jlong h) { usbmem_partscan_close(ps(h)); }
JNIEXPORT jbyteArray JNICALL PS(nativeScanResultJson)(JNIEnv* env, jclass, jlong h, jintArray err) {
  return json_call(env, err, [&](char** o) { return usbmem_partscan_result_json(ps(h), o); });
}
JNIEXPORT jbyteArray JNICALL PS(nativeScanProposalsJson)(JNIEnv* env, jclass, jlong h, jintArray err) {
  return json_call(env, err, [&](char** o) { return usbmem_partscan_proposals_json(ps(h), o); });
}
// 案を編集セッション (PartitionEditor のハンドル) に積む
JNIEXPORT jint JNICALL PS(nativeScanQueueProposal)(JNIEnv*, jclass, jlong h, jint index, jlong editor) {
  return neg(usbmem_partscan_queue_proposal(ps(h), index, reinterpret_cast<usbmem_partedit*>(editor)));
}

// ---- ブートセクタ
JNIEXPORT jlong JNICALL PS(nativeBootOpen)(JNIEnv* env, jclass, jlong dev, jlong offset, jlong length, jbyteArray fsName, jlong job) {
  Utf8 fs(env, fsName);
  usbmem_bootfix* h = nullptr;
  int rc = usbmem_bootfix_open(reinterpret_cast<usbmem_device*>(dev), static_cast<unsigned long long>(offset), static_cast<unsigned long long>(length), fs.c(), jb(job), &h);
  if (rc != 0) return -static_cast<jlong>(rc);
  return reinterpret_cast<jlong>(h);
}
JNIEXPORT void JNICALL PS(nativeBootClose)(JNIEnv*, jclass, jlong h) { usbmem_bootfix_close(bf(h)); }
// "code\nmessage" (UTF-8)。h == 0 なら直近の open の失敗
JNIEXPORT jbyteArray JNICALL PS(nativeBootLastError)(JNIEnv* env, jclass, jlong h) {
  char c[64] = {}, m[512] = {};
  usbmem_bootfix_last_error(bf(h), c, sizeof c, m, sizeof m);
  std::string s = std::string(c) + "\n" + m;
  return to_bytes(env, s.data(), s.size());
}
JNIEXPORT jbyteArray JNICALL PS(nativeBootInspectionJson)(JNIEnv* env, jclass, jlong h, jintArray err) {
  return json_call(env, err, [&](char** o) { return usbmem_bootfix_inspection_json(bf(h), o); });
}
JNIEXPORT jbyteArray JNICALL PS(nativeBootPlanJson)(JNIEnv* env, jclass, jlong h, jbyteArray repairId, jlong source, jintArray err) {
  Utf8 id(env, repairId);
  return json_call(env, err, [&](char** o) { return usbmem_bootfix_plan_json(bf(h), id.c(), static_cast<unsigned long long>(source), o); });
}
JNIEXPORT jbyteArray JNICALL PS(nativeBootMakeToken)(JNIEnv* env, jclass, jlong h, jbyteArray typed, jintArray err) {
  Utf8 t(env, typed);
  return json_call(env, err, [&](char** o) { return usbmem_bootfix_make_token(bf(h), t.c(), o); });
}
// 失敗しても結果 JSON (code / message / rolledBack ...) が返る。err[0] は戻り値 (0 = 成功)
JNIEXPORT jbyteArray JNICALL PS(nativeBootApply)(JNIEnv* env, jclass, jlong h, jbyteArray token, jbyteArray backupPath, jlong timestamp, jboolean inUse, jlong job,
                                                jintArray err) {
  Utf8 t(env, token), bp(env, backupPath);
  return json_call(env, err,
                   [&](char** o) { return usbmem_bootfix_apply(bf(h), t.c(), bp.c(), static_cast<unsigned long long>(timestamp), inUse ? 1 : 0, jb(job), o); }, true);
}
JNIEXPORT jbyteArray JNICALL PS(nativeBootLastBackup)(JNIEnv* env, jclass, jlong h, jintArray err) {
  unsigned char* p = nullptr;
  size_t n = 0;
  int rc = usbmem_bootfix_last_backup(bf(h), &p, &n);
  if (rc != 0 || !p) {
    set_err(env, err, rc);
    if (p) usbmem_free(p);
    return nullptr;
  }
  jbyteArray arr = to_bytes(env, p, n);
  usbmem_free(p);
  return arr;
}
JNIEXPORT jbyteArray JNICALL PS(nativeBootBackupInfo)(JNIEnv* env, jclass, jbyteArray data, jintArray err) {
  if (!data) { set_err(env, err, 1); return nullptr; }
  jsize n = env->GetArrayLength(data);
  std::vector<unsigned char> buf(static_cast<size_t>(n));
  if (n) env->GetByteArrayRegion(data, 0, n, reinterpret_cast<jbyte*>(buf.data()));
  return json_call(env, err, [&](char** o) { return usbmem_bootfix_backup_info_json(buf.data(), buf.size(), o); });
}
JNIEXPORT jint JNICALL PS(nativeBootRestoreBackup)(JNIEnv* env, jclass, jlong dev, jbyteArray data) {
  if (!data) return -1;
  jsize n = env->GetArrayLength(data);
  std::vector<unsigned char> buf(static_cast<size_t>(n));
  if (n) env->GetByteArrayRegion(data, 0, n, reinterpret_cast<jbyte*>(buf.data()));
  return neg(usbmem_bootfix_restore_backup(reinterpret_cast<usbmem_device*>(dev), buf.data(), buf.size()));
}

}  // extern "C"
