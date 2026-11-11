// SPDX-License-Identifier: GPL-2.0-or-later
// NTFS (読み取り専用) の JNI。文字列は UTF-8 byte[] で受け渡す。
#include <jni.h>

#include <memory>
#include <string>

#include "usbmem/devices.h"
#include "usbmem/ntfs_volume.h"

namespace {
struct Vol { std::unique_ptr<usbmem::NtfsVolume> v; };

std::string utf8(JNIEnv* env, jbyteArray a) {
  if (!a) return {};
  jsize n = env->GetArrayLength(a);
  std::string s(static_cast<size_t>(n), '\0');
  if (n) env->GetByteArrayRegion(a, 0, n, reinterpret_cast<jbyte*>(&s[0]));
  return s;
}
void json_str(std::string& o, const std::string& s) {
  o += '"';
  for (unsigned char c : s) {
    if (c == '"' || c == '\\') { o += '\\'; o += static_cast<char>(c); }
    else if (c < 0x20) { char b[8]; snprintf(b, sizeof b, "\\u%04x", c); o += b; }
    else o += static_cast<char>(c);
  }
  o += '"';
}
jbyteArray bytes(JNIEnv* env, const std::string& s) {
  jbyteArray a = env->NewByteArray(static_cast<jsize>(s.size()));
  if (a) env->SetByteArrayRegion(a, 0, static_cast<jsize>(s.size()), reinterpret_cast<const jbyte*>(s.data()));
  return a;
}
}  // namespace

extern "C" {

// fd の所有権を引き取り、デバイス全体を NTFS として読み取り専用マウント。戻り値: >0 ハンドル / <=0 = -errno
JNIEXPORT jlong JNICALL Java_org_usbmem_app_NativeBridge_nativeNtfsMount(JNIEnv*, jclass, jint fd) {
  usbmem::BlockDevicePtr dev;
  usbmem::Status st = usbmem::RawImageDevice::open_fd(fd, true, true, 512, &dev);
  if (!st.ok()) return -22;
  int err = 0;
  auto v = usbmem::NtfsVolume::mount(dev, &err);
  if (!v) return err > 0 ? -static_cast<jlong>(err) : -5;
  return reinterpret_cast<jlong>(new Vol{std::move(v)});
}

JNIEXPORT void JNICALL Java_org_usbmem_app_NativeBridge_nativeNtfsUnmount(JNIEnv*, jclass, jlong h) {
  delete reinterpret_cast<Vol*>(h);
}

// 一覧を JSON で返す: [{"name":..,"ino":"..","dir":bool,"size":"..","mtime":..}]。失敗時 null (err[0] = errno)
JNIEXPORT jbyteArray JNICALL Java_org_usbmem_app_NativeBridge_nativeNtfsList(JNIEnv* env, jclass, jlong h,
                                                                           jbyteArray path, jintArray err) {
  auto* v = reinterpret_cast<Vol*>(h)->v.get();
  uint64_t ino = 0;
  int rc = v->resolve(utf8(env, path), &ino);
  std::vector<usbmem::NtfsDirEntry> es;
  if (!rc) rc = v->readdir(ino, &es);
  if (rc) { jint e = rc < 0 ? -rc : rc; env->SetIntArrayRegion(err, 0, 1, &e); return nullptr; }
  std::string o = "[";
  bool first = true;
  for (auto& e : es) {
    if (e.name == "." || e.name == "..") continue;
    usbmem::NtfsStat st;
    if (v->stat(e.ino, &st)) continue;
    if (!first) o += ',';
    first = false;
    o += "{\"name\":"; json_str(o, e.name);
    o += ",\"ino\":\"" + std::to_string(e.ino) + "\",\"dir\":" + (st.is_dir() ? "true" : "false");
    o += ",\"size\":\"" + std::to_string(st.size) + "\",\"mtime\":" + std::to_string(st.mtime) + "}";
  }
  o += "]";
  return bytes(env, o);
}

// 単一エントリの情報を JSON で返す: {"ino":"..","dir":bool,"size":"..","mtime":..}。失敗時 null (err[0] = errno)
JNIEXPORT jbyteArray JNICALL Java_org_usbmem_app_NativeBridge_nativeNtfsStat(JNIEnv* env, jclass, jlong h,
                                                                           jbyteArray path, jintArray err) {
  auto* v = reinterpret_cast<Vol*>(h)->v.get();
  uint64_t ino = 0;
  usbmem::NtfsStat st;
  int rc = v->resolve(utf8(env, path), &ino);
  if (!rc) rc = v->stat(ino, &st);
  if (rc) { jint e = rc < 0 ? -rc : rc; env->SetIntArrayRegion(err, 0, 1, &e); return nullptr; }
  std::string o = "{\"ino\":\"" + std::to_string(ino) + "\",\"dir\":" + (st.is_dir() ? "true" : "false") +
                  ",\"size\":\"" + std::to_string(st.size) + "\",\"mtime\":" + std::to_string(st.mtime) + "}";
  return bytes(env, o);
}

// path のファイルを off から読む。戻り値: 読めたバイト数 / <0 = -errno
JNIEXPORT jlong JNICALL Java_org_usbmem_app_NativeBridge_nativeNtfsRead(JNIEnv* env, jclass, jlong h, jbyteArray path,
                                                                      jlong off, jbyteArray buf) {
  auto* v = reinterpret_cast<Vol*>(h)->v.get();
  uint64_t ino = 0;
  int rc = v->resolve(utf8(env, path), &ino);
  if (rc) return rc < 0 ? rc : -rc;
  jsize n = env->GetArrayLength(buf);
  std::string tmp(static_cast<size_t>(n), '\0');
  int64_t r = v->read(ino, static_cast<uint64_t>(off), n ? &tmp[0] : nullptr, static_cast<size_t>(n));
  if (r > 0) env->SetByteArrayRegion(buf, 0, static_cast<jsize>(r), reinterpret_cast<const jbyte*>(tmp.data()));
  return r;
}

}  // extern "C"
