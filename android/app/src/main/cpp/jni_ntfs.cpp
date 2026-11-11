// SPDX-License-Identifier: GPL-2.0-or-later
// NTFS (読み取り専用) の JNI。文字列は UTF-8 byte[] で受け渡す。
#include <jni.h>

#include <memory>
#include <string>

#include "usbmem/c_api_internal.h"
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

// 書き込み可能でマウント (実験的。ビルドで USBMEM_NTFS_WRITE が無効なら -EROFS)。fd の所有権を引き取る。戻り値: >0 ハンドル / <=0 = -errno
JNIEXPORT jlong JNICALL Java_org_usbmem_app_NativeBridge_nativeNtfsMountRw(JNIEnv*, jclass, jint fd) {
  usbmem::BlockDevicePtr dev;
  usbmem::Status st = usbmem::RawImageDevice::open_fd(fd, false, true, 512, &dev);
  if (!st.ok()) return -22;
  int err = 0;
  auto v = usbmem::NtfsVolume::mount_rw(dev, &err);
  if (!v) return err > 0 ? -static_cast<jlong>(err) : -5;
  return reinterpret_cast<jlong>(new Vol{std::move(v)});
}

// 上流の判断で読み取り専用に落ちていないか (1 = 書き込み可)
JNIEXPORT jboolean JNICALL Java_org_usbmem_app_NativeBridge_nativeNtfsWritable(JNIEnv*, jclass, jlong h) {
  return reinterpret_cast<Vol*>(h)->v->writable() ? JNI_TRUE : JNI_FALSE;
}

// 既に開いているデバイス (USB 等) の [offset, offset+length) を NTFS として読み取り専用マウント。ハンドルは消費しない
// (呼び出し側が NTFS を unmount してからデバイスを close する)。戻り値: >0 ハンドル / <=0 = -errno
JNIEXPORT jlong JNICALL Java_org_usbmem_app_NativeBridge_nativeNtfsMountDevice(JNIEnv*, jclass, jlong devHandle, jlong offset, jlong length) {
  auto* d = reinterpret_cast<usbmem_device*>(devHandle);
  if (!d || offset < 0 || length <= 0) return -22;
  usbmem::BlockDevicePtr sub;
  usbmem::Status st = usbmem::SubRangeDevice::create(d->dev, static_cast<uint64_t>(offset), static_cast<uint64_t>(length), true, &sub);
  if (!st.ok()) return -22;
  int err = 0;
  auto v = usbmem::NtfsVolume::mount(sub, &err);
  if (!v) return err > 0 ? -static_cast<jlong>(err) : -5;
  return reinterpret_cast<jlong>(new Vol{std::move(v)});
}

// 開いているデバイスの一区間を書き込み可能で NTFS マウント (実験的。ビルドで無効なら -EROFS)。デバイス自体も read_only=false で開かれていること。
JNIEXPORT jlong JNICALL Java_org_usbmem_app_NativeBridge_nativeNtfsMountDeviceRw(JNIEnv*, jclass, jlong devHandle, jlong offset, jlong length) {
  auto* d = reinterpret_cast<usbmem_device*>(devHandle);
  if (!d || offset < 0 || length <= 0) return -22;
  usbmem::BlockDevicePtr sub;
  usbmem::Status st = usbmem::SubRangeDevice::create(d->dev, static_cast<uint64_t>(offset), static_cast<uint64_t>(length), false, &sub);
  if (!st.ok()) return -22;
  int err = 0;
  auto v = usbmem::NtfsVolume::mount_rw(sub, &err);
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

namespace {
// "/a/b" -> ("/a", "b"); "/x" -> ("/", "x")
void split_path(const std::string& p, std::string* dir, std::string* name) {
  size_t i = p.find_last_of('/');
  if (i == std::string::npos) { *dir = "/"; *name = p; return; }
  *dir = i == 0 ? "/" : p.substr(0, i); *name = p.substr(i + 1);
}
inline jlong neg(int rc) { return rc < 0 ? rc : -rc; }
}  // namespace

extern "C" {

// path のファイルへ off から書く。戻り値: 書けたバイト数 / <0 = -errno
JNIEXPORT jlong JNICALL Java_org_usbmem_app_NativeBridge_nativeNtfsWrite(JNIEnv* env, jclass, jlong h, jbyteArray path,
                                                                       jlong off, jbyteArray buf, jint len) {
  auto* v = reinterpret_cast<Vol*>(h)->v.get();
  uint64_t ino = 0;
  int rc = v->resolve(utf8(env, path), &ino);
  if (rc) return neg(rc);
  if (len < 0 || len > env->GetArrayLength(buf) || off < 0) return -22;
  std::string tmp(static_cast<size_t>(len), '\0');
  if (len) env->GetByteArrayRegion(buf, 0, len, reinterpret_cast<jbyte*>(&tmp[0]));
  return v->write(ino, static_cast<uint64_t>(off), tmp.data(), static_cast<size_t>(len));
}

// parent ディレクトリに name のファイル/ディレクトリを作る。戻り値: ino / <0 = -errno
JNIEXPORT jlong JNICALL Java_org_usbmem_app_NativeBridge_nativeNtfsCreate(JNIEnv* env, jclass, jlong h, jbyteArray parent,
                                                                        jbyteArray name, jboolean isDir) {
  auto* v = reinterpret_cast<Vol*>(h)->v.get();
  uint64_t dir = 0, ino = 0;
  int rc = v->resolve(utf8(env, parent), &dir);
  if (!rc) rc = isDir ? v->mkdir(dir, utf8(env, name), 0755, &ino) : v->create(dir, utf8(env, name), 0644, &ino);
  return rc ? neg(rc) : static_cast<jlong>(ino);
}

// path を削除 (ファイルは unlink、ディレクトリは空なら rmdir)。0 / <0 = -errno
JNIEXPORT jlong JNICALL Java_org_usbmem_app_NativeBridge_nativeNtfsRemove(JNIEnv* env, jclass, jlong h, jbyteArray path) {
  auto* v = reinterpret_cast<Vol*>(h)->v.get();
  std::string p = utf8(env, path), dir, name;
  split_path(p, &dir, &name);
  uint64_t d = 0, ino = 0;
  usbmem::NtfsStat st;
  int rc = v->resolve(dir, &d);
  if (!rc) rc = v->resolve(p, &ino);
  if (!rc) rc = v->stat(ino, &st);
  if (!rc) rc = st.is_dir() ? v->rmdir(d, name) : v->unlink(d, name);
  return rc ? neg(rc) : 0;
}

// 同じディレクトリ内での名前変更。0 / <0 = -errno
JNIEXPORT jlong JNICALL Java_org_usbmem_app_NativeBridge_nativeNtfsRename(JNIEnv* env, jclass, jlong h, jbyteArray path, jbyteArray newName) {
  auto* v = reinterpret_cast<Vol*>(h)->v.get();
  std::string dir, name;
  split_path(utf8(env, path), &dir, &name);
  uint64_t d = 0;
  int rc = v->resolve(dir, &d);
  if (!rc) rc = v->rename(d, name, d, utf8(env, newName));
  return rc ? neg(rc) : 0;
}

JNIEXPORT jlong JNICALL Java_org_usbmem_app_NativeBridge_nativeNtfsTruncate(JNIEnv* env, jclass, jlong h, jbyteArray path, jlong size) {
  auto* v = reinterpret_cast<Vol*>(h)->v.get();
  uint64_t ino = 0;
  if (size < 0) return -22;
  int rc = v->resolve(utf8(env, path), &ino);
  if (!rc) rc = v->truncate(ino, static_cast<uint64_t>(size));
  return rc ? neg(rc) : 0;
}

// 変更をメディアへ確定 (書き込み + デバイス flush)。0 / <0 = -errno
JNIEXPORT jlong JNICALL Java_org_usbmem_app_NativeBridge_nativeNtfsSync(JNIEnv*, jclass, jlong h) {
  int rc = reinterpret_cast<Vol*>(h)->v->sync();
  return rc ? neg(rc) : 0;
}

}  // extern "C"
