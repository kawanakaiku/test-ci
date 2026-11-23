// SPDX-License-Identifier: Apache-2.0
// 削除ファイルの一覧と救出 / カービングの JNI (対応する Kotlin は Undelete.kt)。jni_partscan.cpp と同じ作法:
// 例外を出さず、文字列は UTF-8 の byte[] で受け渡し、失敗は 0 以外の戻り値 (-(usbmem::Errc)) か null + errorOut。
// ハンドルはポインタをそのまま jlong にしたもの (>4095。エラーは小さな負の値)。
#include <jni.h>

#include <cstdlib>
#include <cstring>
#include <string>

#include "usbmem/c_api_undelete.h"

namespace {

usbmem_ujob* jb(jlong h) { return reinterpret_cast<usbmem_ujob*>(h); }
usbmem_undelete* ud(jlong h) { return reinterpret_cast<usbmem_undelete*>(h); }
usbmem_carve* cv(jlong h) { return reinterpret_cast<usbmem_carve*>(h); }
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

void set_rc(JNIEnv* env, jintArray a, int rc) {
  if (a && env->GetArrayLength(a) >= 1) {
    jint e = rc;
    env->SetIntArrayRegion(a, 0, 1, &e);
  }
}

// JSON を返す C API の共通ラッパ。戻り値 (0 = 成功) は常に err[0] へ。keep_on_error = true なら、戻り値が 0 でなくても JSON があれば返す
// (抽出の失敗理由が JSON に入っている)。
template <typename F>
jbyteArray json_call(JNIEnv* env, jintArray err, F f, bool keep_on_error = false) {
  char* out = nullptr;
  int rc = f(&out);
  set_rc(env, err, rc);
  if ((rc != 0 && !keep_on_error) || !out) {
    if (rc == 0) set_rc(env, err, 12);
    if (out) usbmem_free(out);
    return nullptr;
  }
  jbyteArray arr = to_bytes(env, out, std::strlen(out));
  usbmem_free(out);
  return arr;
}

}  // namespace

#define UD(name) Java_org_usbmem_app_Undelete_##name

extern "C" {

// ---- 進捗とキャンセル (別スレッドから呼んでよい)
JNIEXPORT jlong JNICALL UD(nativeJobNew)(JNIEnv*, jclass) { return reinterpret_cast<jlong>(usbmem_ujob_new()); }
JNIEXPORT void JNICALL UD(nativeJobFree)(JNIEnv*, jclass, jlong j) { usbmem_ujob_free(jb(j)); }
JNIEXPORT void JNICALL UD(nativeJobCancel)(JNIEnv*, jclass, jlong j) { usbmem_ujob_cancel(jb(j)); }
JNIEXPORT jbyteArray JNICALL UD(nativeJobProgress)(JNIEnv* env, jclass, jlong j, jlongArray out) {
  char ph[32] = {};
  unsigned long long d = 0, t = 0;
  usbmem_ujob_progress(jb(j), ph, sizeof ph, &d, &t);
  if (out && env->GetArrayLength(out) >= 2) {
    jlong v[2] = {static_cast<jlong>(d), static_cast<jlong>(t)};
    env->SetLongArrayRegion(out, 0, 2, v);
  }
  return to_bytes(env, ph, std::strlen(ph));
}

// ---- 削除ファイル。戻り値: >4095 ハンドル / <=0 = -(Errc)。キャンセルされても途中までの結果のハンドルを返し、outRc[0] = Errc::Cancelled
JNIEXPORT jlong JNICALL UD(nativeUndeleteScan)(JNIEnv* env, jclass, jlong dev, jboolean includeEmpty, jlong job, jintArray outRc) {
  usbmem_undelete* h = nullptr;
  int rc = usbmem_undelete_scan(dv(dev), includeEmpty ? 1 : 0, jb(job), &h);
  set_rc(env, outRc, rc);
  if (!h) return -static_cast<jlong>(rc ? rc : 12);
  return reinterpret_cast<jlong>(h);
}
JNIEXPORT void JNICALL UD(nativeUndeleteClose)(JNIEnv*, jclass, jlong h) { usbmem_undelete_close(ud(h)); }
JNIEXPORT jbyteArray JNICALL UD(nativeUndeleteResultJson)(JNIEnv* env, jclass, jlong h, jintArray err) {
  return json_call(env, err, [&](char** o) { return usbmem_undelete_result_json(ud(h), o); });
}
// 失敗・取り消しでも結果 JSON ({"ok":false,"code":..}) が返る。err[0] は戻り値
JNIEXPORT jbyteArray JNICALL UD(nativeUndeleteExtractFd)(JNIEnv* env, jclass, jlong h, jint id, jint fd, jlong job, jintArray err) {
  return json_call(env, err, [&](char** o) { return usbmem_undelete_extract_fd(ud(h), static_cast<unsigned>(id), fd, jb(job), o); }, true);
}
JNIEXPORT jbyteArray JNICALL UD(nativeUndeleteExtractPath)(JNIEnv* env, jclass, jlong h, jint id, jbyteArray path, jlong job, jintArray err) {
  Utf8 p(env, path);
  if (!p.has) { set_rc(env, err, 1); return nullptr; }
  return json_call(env, err, [&](char** o) { return usbmem_undelete_extract_path(ud(h), static_cast<unsigned>(id), p.c(), jb(job), o); }, true);
}

// ---- カービング
JNIEXPORT jbyteArray JNICALL UD(nativeCarveTypesJson)(JNIEnv* env, jclass, jintArray err) {
  return json_call(env, err, [&](char** o) { return usbmem_carve_types_json(o); });
}
// 戻り値: >4095 ハンドル / <=0 = -(Errc)。キャンセルされても途中までの結果のハンドルを返し、outRc[0] = Errc::Cancelled (statePath から再開できる)
JNIEXPORT jlong JNICALL UD(nativeCarveRun)(JNIEnv* env, jclass, jlong dev, jbyteArray types, jlong minSize, jlong maxSize, jint align, jboolean unallocatedOnly,
                                          jlong maxHits, jbyteArray statePath, jbyteArray outDir, jlong job, jintArray outRc) {
  Utf8 t(env, types), sp(env, statePath), od(env, outDir);
  usbmem_carve_options o;
  std::memset(&o, 0, sizeof o);
  o.min_size = static_cast<unsigned long long>(minSize);
  o.max_size = static_cast<unsigned long long>(maxSize);
  o.align = static_cast<unsigned>(align);
  o.unallocated_only = unallocatedOnly ? 1 : 0;
  o.max_hits = static_cast<unsigned long long>(maxHits);
  usbmem_carve* h = nullptr;
  int rc = usbmem_carve_run(dv(dev), t.c(), &o, sp.c(), od.c(), jb(job), &h);
  set_rc(env, outRc, rc);
  if (!h) return -static_cast<jlong>(rc ? rc : 12);
  return reinterpret_cast<jlong>(h);
}
JNIEXPORT void JNICALL UD(nativeCarveClose)(JNIEnv*, jclass, jlong h) { usbmem_carve_close(cv(h)); }
JNIEXPORT jbyteArray JNICALL UD(nativeCarveResultJson)(JNIEnv* env, jclass, jlong h, jintArray err) {
  return json_call(env, err, [&](char** o) { return usbmem_carve_result_json(cv(h), o); });
}
JNIEXPORT jbyteArray JNICALL UD(nativeCarveExtractFd)(JNIEnv* env, jclass, jlong h, jint index, jint fd, jlong job, jintArray err) {
  return json_call(env, err, [&](char** o) { return usbmem_carve_extract_fd(cv(h), static_cast<unsigned>(index), fd, jb(job), o); }, true);
}
JNIEXPORT jbyteArray JNICALL UD(nativeCarveExtractPath)(JNIEnv* env, jclass, jlong h, jint index, jbyteArray path, jlong job, jintArray err) {
  Utf8 p(env, path);
  if (!p.has) { set_rc(env, err, 1); return nullptr; }
  return json_call(env, err, [&](char** o) { return usbmem_carve_extract_path(cv(h), static_cast<unsigned>(index), p.c(), jb(job), o); }, true);
}

}  // extern "C"
