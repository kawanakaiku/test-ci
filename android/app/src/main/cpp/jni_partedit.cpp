// SPDX-License-Identifier: Apache-2.0
// パーティション編集の JNI (対応する Kotlin は PartitionEditor.kt)。jni_bridge.cpp と同じ作法:
// 例外を出さず、文字列は UTF-8 の byte[] で受け渡し、失敗は 0 以外の戻り値 (-(usbmem::Errc)) か null + errorOut。
// ハンドルはポインタをそのまま jlong にしたもの (>0 かつ 4095 より大きい。エラーは小さな負の値)。
#include <jni.h>

#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include "usbmem/c_api_partedit.h"

namespace {

usbmem_partedit* ed(jlong h) { return reinterpret_cast<usbmem_partedit*>(h); }

// byte[] (UTF-8) → std::string。null は has=false
struct Utf8 {
  std::string s;
  bool has = false;
  Utf8(JNIEnv* env, jbyteArray a) {
    if (!a) return;
    has = true;
    jsize n = env->GetArrayLength(a);
    s.resize(static_cast<size_t>(n));
    if (n) env->GetByteArrayRegion(a, 0, n, reinterpret_cast<jbyte*>(&s[0]));
    // C 側は NUL 終端文字列として扱うので、途中に NUL があれば切る
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

// 文字列を返す C API の共通ラッパ: 成功なら UTF-8 byte[]、失敗なら null と errorOut[0]
template <typename F>
jbyteArray json_call(JNIEnv* env, jintArray errorOut, F f) {
  char* out = nullptr;
  int rc = f(&out);
  if (rc != 0 || !out) {
    if (errorOut && env->GetArrayLength(errorOut) >= 1) {
      jint e = rc ? rc : 12;
      env->SetIntArrayRegion(errorOut, 0, 1, &e);
    }
    if (out) usbmem_free(out);
    return nullptr;
  }
  jbyteArray arr = to_bytes(env, out, std::strlen(out));
  usbmem_free(out);
  return arr;
}

jint neg(int rc) { return rc ? -rc : 0; }

}  // namespace

#define PE(name) Java_org_usbmem_app_PartitionEditor_##name

extern "C" {

// 戻り値: >4095 ハンドル / <=0 = -(Errc)。devHandle は NativeBridge が返したデバイスのハンドル
JNIEXPORT jlong JNICALL PE(nativeOpen)(JNIEnv*, jclass, jlong devHandle, jint sectorSize, jboolean inUse, jlong rngSeed) {
  usbmem_partedit* h = nullptr;
  int rc = usbmem_partedit_open(reinterpret_cast<usbmem_device*>(devHandle), static_cast<unsigned>(sectorSize), inUse ? 1 : 0,
                                static_cast<unsigned long long>(rngSeed), &h);
  if (rc != 0) return -static_cast<jlong>(rc);
  return reinterpret_cast<jlong>(h);
}

JNIEXPORT void JNICALL PE(nativeClose)(JNIEnv*, jclass, jlong h) { usbmem_partedit_close(ed(h)); }
JNIEXPORT void JNICALL PE(nativeSetInUse)(JNIEnv*, jclass, jlong h, jboolean inUse) { usbmem_partedit_set_in_use(ed(h), inUse ? 1 : 0); }

// "code\nmessage" (UTF-8)。h == 0 なら直近の open の失敗
JNIEXPORT jbyteArray JNICALL PE(nativeLastError)(JNIEnv* env, jclass, jlong h) {
  char c[64] = {}, m[512] = {};
  usbmem_partedit_last_error(ed(h), c, sizeof c, m, sizeof m);
  std::string s = std::string(c) + "\n" + m;
  return to_bytes(env, s.data(), s.size());
}

JNIEXPORT jbyteArray JNICALL PE(nativeTableJson)(JNIEnv* env, jclass, jlong h, jintArray err) {
  return json_call(env, err, [&](char** o) { return usbmem_partedit_table_json(ed(h), o); });
}
JNIEXPORT jbyteArray JNICALL PE(nativeGptJson)(JNIEnv* env, jclass, jlong h, jintArray err) {
  return json_call(env, err, [&](char** o) { return usbmem_partedit_gpt_json(ed(h), o); });
}

// 作ったパーティションのハンドルは outHandle[0] に入る。戻り値は 0 / -Errc
JNIEXPORT jint JNICALL PE(nativeCreate)(JNIEnv* env, jclass, jlong h, jint role, jlong start, jlong count, jint mbrType, jbyteArray typeGuid,
                                       jbyteArray uniqueGuid, jbyteArray name, jlong attrs, jboolean bootable, jintArray outHandle) {
  Utf8 tg(env, typeGuid), ug(env, uniqueGuid), nm(env, name);
  unsigned hv = 0;
  int rc = usbmem_partedit_create(ed(h), role, static_cast<unsigned long long>(start), static_cast<unsigned long long>(count), mbrType, tg.c(),
                                  ug.c(), nm.c(), static_cast<unsigned long long>(attrs), bootable ? 1 : 0, &hv);
  if (rc == 0 && outHandle && env->GetArrayLength(outHandle) >= 1) {
    jint v = static_cast<jint>(hv);
    env->SetIntArrayRegion(outHandle, 0, 1, &v);
  }
  return neg(rc);
}

JNIEXPORT jint JNICALL PE(nativeRemove)(JNIEnv*, jclass, jlong h, jint part) { return neg(usbmem_partedit_remove(ed(h), static_cast<unsigned>(part))); }
JNIEXPORT jint JNICALL PE(nativeResize)(JNIEnv*, jclass, jlong h, jint part, jlong count) {
  return neg(usbmem_partedit_resize(ed(h), static_cast<unsigned>(part), static_cast<unsigned long long>(count)));
}
JNIEXPORT jint JNICALL PE(nativeMove)(JNIEnv*, jclass, jlong h, jint part, jlong start) {
  return neg(usbmem_partedit_move(ed(h), static_cast<unsigned>(part), static_cast<unsigned long long>(start)));
}
JNIEXPORT jint JNICALL PE(nativeSetMbrType)(JNIEnv*, jclass, jlong h, jint part, jint type) {
  return neg(usbmem_partedit_set_mbr_type(ed(h), static_cast<unsigned>(part), type));
}
JNIEXPORT jint JNICALL PE(nativeSetGptType)(JNIEnv* env, jclass, jlong h, jint part, jbyteArray guid) {
  Utf8 g(env, guid);
  return neg(usbmem_partedit_set_gpt_type(ed(h), static_cast<unsigned>(part), g.c()));
}
JNIEXPORT jint JNICALL PE(nativeSetName)(JNIEnv* env, jclass, jlong h, jint part, jbyteArray name) {
  Utf8 n(env, name);
  return neg(usbmem_partedit_set_name(ed(h), static_cast<unsigned>(part), n.c()));
}
JNIEXPORT jint JNICALL PE(nativeSetAttrs)(JNIEnv*, jclass, jlong h, jint part, jlong attrs) {
  return neg(usbmem_partedit_set_attrs(ed(h), static_cast<unsigned>(part), static_cast<unsigned long long>(attrs)));
}
JNIEXPORT jint JNICALL PE(nativeSetBootable)(JNIEnv*, jclass, jlong h, jint part, jboolean b) {
  return neg(usbmem_partedit_set_bootable(ed(h), static_cast<unsigned>(part), b ? 1 : 0));
}
JNIEXPORT jint JNICALL PE(nativeSetUniqueGuid)(JNIEnv* env, jclass, jlong h, jint part, jbyteArray guid) {
  Utf8 g(env, guid);
  return neg(usbmem_partedit_set_unique_guid(ed(h), static_cast<unsigned>(part), g.c()));
}
JNIEXPORT jint JNICALL PE(nativeSetDiskId)(JNIEnv* env, jclass, jlong h, jbyteArray id) {
  Utf8 s(env, id);
  return neg(usbmem_partedit_set_disk_id(ed(h), s.c()));
}
JNIEXPORT jint JNICALL PE(nativeNewTable)(JNIEnv*, jclass, jlong h, jint kind, jint entries, jboolean overwriteFs) {
  return neg(usbmem_partedit_new_table(ed(h), kind, static_cast<unsigned>(entries), overwriteFs ? 1 : 0));
}
JNIEXPORT jint JNICALL PE(nativeRepairGpt)(JNIEnv*, jclass, jlong h, jint kind) { return neg(usbmem_partedit_repair_gpt(ed(h), kind)); }
JNIEXPORT void JNICALL PE(nativeClearOps)(JNIEnv*, jclass, jlong h) { usbmem_partedit_clear_ops(ed(h)); }

JNIEXPORT jbyteArray JNICALL PE(nativePlanJson)(JNIEnv* env, jclass, jlong h, jboolean includeSectors, jintArray err) {
  return json_call(env, err, [&](char** o) { return usbmem_partedit_plan_json(ed(h), includeSectors ? 1 : 0, o); });
}
JNIEXPORT jbyteArray JNICALL PE(nativeMakeToken)(JNIEnv* env, jclass, jlong h, jbyteArray typed, jintArray err) {
  Utf8 t(env, typed);
  return json_call(env, err, [&](char** o) { return usbmem_partedit_make_token(ed(h), t.c(), o); });
}
JNIEXPORT jbyteArray JNICALL PE(nativeApply)(JNIEnv* env, jclass, jlong h, jbyteArray token, jbyteArray backupPath, jlong timestamp, jintArray err) {
  Utf8 t(env, token), bp(env, backupPath);
  // 失敗でも結果 JSON (code / message / rolledBack ...) が返るので、JSON があれば常に返す。errorOut[0] には戻り値を入れる
  char* out = nullptr;
  int rc = usbmem_partedit_apply(ed(h), t.c(), bp.c(), static_cast<unsigned long long>(timestamp), &out);
  if (err && env->GetArrayLength(err) >= 1) {
    jint e = rc;
    env->SetIntArrayRegion(err, 0, 1, &e);
  }
  if (!out) return nullptr;
  jbyteArray arr = to_bytes(env, out, std::strlen(out));
  usbmem_free(out);
  return arr;
}

namespace {
jbyteArray bytes_call(JNIEnv* env, jintArray err, int rc, unsigned char* p, size_t n) {
  if (rc != 0 || !p) {
    if (err && env->GetArrayLength(err) >= 1) {
      jint e = rc ? rc : 12;
      env->SetIntArrayRegion(err, 0, 1, &e);
    }
    if (p) usbmem_free(p);
    return nullptr;
  }
  jbyteArray arr = to_bytes(env, p, n);
  usbmem_free(p);
  return arr;
}
}  // namespace

JNIEXPORT jbyteArray JNICALL PE(nativeBackup)(JNIEnv* env, jclass, jlong h, jlong timestamp, jintArray err) {
  unsigned char* p = nullptr;
  size_t n = 0;
  int rc = usbmem_partedit_backup(ed(h), static_cast<unsigned long long>(timestamp), &p, &n);
  return bytes_call(env, err, rc, p, n);
}
JNIEXPORT jbyteArray JNICALL PE(nativeLastBackup)(JNIEnv* env, jclass, jlong h, jintArray err) {
  unsigned char* p = nullptr;
  size_t n = 0;
  int rc = usbmem_partedit_last_backup(ed(h), &p, &n);
  return bytes_call(env, err, rc, p, n);
}
JNIEXPORT jbyteArray JNICALL PE(nativeBackupInfo)(JNIEnv* env, jclass, jbyteArray data, jintArray err) {
  if (!data) return nullptr;
  jsize n = env->GetArrayLength(data);
  std::vector<unsigned char> buf(static_cast<size_t>(n) + 1);
  if (n) env->GetByteArrayRegion(data, 0, n, reinterpret_cast<jbyte*>(buf.data()));
  return json_call(env, err, [&](char** o) { return usbmem_partedit_backup_info_json(buf.data(), static_cast<size_t>(n), o); });
}
JNIEXPORT jbyteArray JNICALL PE(nativePlanRestore)(JNIEnv* env, jclass, jlong h, jbyteArray data, jintArray err) {
  if (!data) return nullptr;
  jsize n = env->GetArrayLength(data);
  std::vector<unsigned char> buf(static_cast<size_t>(n) + 1);
  if (n) env->GetByteArrayRegion(data, 0, n, reinterpret_cast<jbyte*>(buf.data()));
  return json_call(env, err, [&](char** o) { return usbmem_partedit_plan_restore(ed(h), buf.data(), static_cast<size_t>(n), o); });
}
JNIEXPORT jbyteArray JNICALL PE(nativeExportSfdisk)(JNIEnv* env, jclass, jlong h, jbyteArray deviceName, jintArray err) {
  Utf8 d(env, deviceName);
  return json_call(env, err, [&](char** o) { return usbmem_partedit_export_sfdisk(ed(h), d.c(), o); });
}

}  // extern "C"
