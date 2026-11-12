// SPDX-License-Identifier: GPL-2.0-or-later
// NTFS (読み取り専用) の JNI。文字列は UTF-8 byte[] で受け渡す。
#include <jni.h>
#include <android/log.h>
#include <cerrno>
#include <chrono>
#include <ctime>

#include <memory>
#include <string>

#include "usbmem/c_api_internal.h"
#include "usbmem/devices.h"
#include "usbmem/ext2_volume.h"
#include "usbmem/ext4_volume.h"
#include "usbmem/exfat_volume.h"
#include "usbmem/fat_c.h"
#include "usbmem/fat_volume.h"
#include "usbmem/fs_probe.h"
#include "usbmem/isofs_volume.h"
#include "usbmem/udf_volume.h"
#include "usbmem/ntfs_volume.h"
#include "usbmem/squashfs_volume.h"
#include "usbmem/erofs_volume.h"
#include "usbmem/btrfs_volume.h"
#include "usbmem/f2fs_volume.h"
#include "usbmem/xfs_volume.h"

#include "jni_vol.h"

namespace {

// dev 全体のファイルシステムを判別してマウントする。exFAT / ext2 / ext3 / ext4 / FAT12・16・32 と判別できたときだけそれぞれ(FAT は読み取り専用で)マウントする。それ以外は従来どおり NTFS として試す
// (壊れた NTFS のブート領域でも、NTFS ドライバ側の検査・復旧の挙動を変えないため)。失敗時 nullptr と正の errno。
std::unique_ptr<usbmem::FsVolume> mount_auto(const usbmem::BlockDevicePtr& dev, bool rw, int* err, bool allow_noload = false) {
  if (dev->is_read_only()) rw = false;  // 下のデバイスが読み取り専用 (ネストの子など) なら、書き込み可を要求されても読み取り専用でマウントする
  usbmem::FsType t = usbmem::probe_filesystem(*dev, 0, dev->size());
  if (t == usbmem::FsType::Exfat) {
    // rw を要求されたときだけ mount_rw (ビルドで USBMEM_NTFS_WRITE が無効なら -EROFS で失敗)。汚れた (VolumeDirty) / MediaFailure のボリュームは
    // mount_rw の中で読み取り専用に落ちる (writable() == false で分かる)
    return rw ? usbmem::ExfatVolume::mount_rw(dev, err) : usbmem::ExfatVolume::mount(dev, err);
  }
  if (t == usbmem::FsType::Ext2) {
    // ext2 (ジャーナルなし・extent なし) は fs/ext2。rw を要求されたときだけ mount_rw (ビルドで USBMEM_NTFS_WRITE が無効なら -EROFS で失敗)。
    // 汚れた / エラー状態 / 未知の ro_compat のボリュームは mount_rw の中で読み取り専用に落ちる (writable() == false で分かる)
    return rw ? usbmem::Ext2Volume::mount_rw(dev, err) : usbmem::Ext2Volume::mount(dev, err);
  }
  if (t == usbmem::FsType::Ext3 || t == usbmem::FsType::Ext4) {
    // ext3 / ext4 は fs/ext4 (+ fs/jbd2)。NTFS として試さない。docs/ext4-write.md
    //  - rw を要求されたときだけ mount_rw (ビルドで USBMEM_NTFS_WRITE が無効なら -EROFS で失敗)。未再生のジャーナル (needs_recovery) はここで再生する。
    //    書き込みのホワイトリスト外の機能・エラー状態・外部ジャーナルなどは mount_rw の中で読み取り専用に落ちる (writable() == false)。
    //  - 読み取り専用: ジャーナルは再生しない。未再生 (needs_recovery) のボリュームは EUCLEAN(117) で拒否、暗号化 / casefold / fs-verity などは EOPNOTSUPP(95)。
    //    allow_noload は、ユーザーが「ジャーナルが未再生で内容が古い/欠けている可能性がある」ことを確認したうえで、読み取り専用で開く場合だけ true
    if (rw && !allow_noload) return usbmem::Ext4Volume::mount_rw(dev, err);
    usbmem::Ext4MountOptions o4; o4.allow_noload = allow_noload;
    return usbmem::Ext4Volume::mount(dev, err, o4);
  }
  if (t == usbmem::FsType::Fat12 || t == usbmem::FsType::Fat16 || t == usbmem::FsType::Fat32) {
    // FAT12/16/32 (vfat の長い名前を含む) は fs/fat で読み取り専用。書き込みは未対応 (rw を要求されても読み取り専用)。NTFS として試さない。
    // FAT の更新時刻は書いた端末のローカル時刻 (タイムゾーン情報なし) なので、現在の端末のタイムゾーンで UTC に直す (UTC より何分西か)
    time_t now = time(nullptr);
    struct tm lt;
    if (localtime_r(&now, &lt)) usbmem_fat_set_timezone(static_cast<int>(-lt.tm_gmtoff / 60));
    return usbmem::FatVolume::mount(dev, err);
  }
  if (t == usbmem::FsType::Udf || t == usbmem::FsType::Iso9660) {
    // 光学メディア (読み取り専用)。ISO 9660 + UDF のブリッジ盤は UDF を優先し、UDF として開けなければ (対応外のパーティションなど) ISO 9660 にフォールバックする。docs/isofs-port.md / docs/udf-port.md
    if (t == usbmem::FsType::Udf) {
      int uerr = 0;
      if (auto u = usbmem::UdfVolume::mount(dev, &uerr)) return u;
      if (err) *err = uerr;
    }
    int ierr = 0;
    if (auto i = usbmem::IsofsVolume::mount(dev, &ierr)) return i;
    if (err && t == usbmem::FsType::Iso9660) *err = ierr;
    return nullptr;
  }
  if (t == usbmem::FsType::Squashfs) {
    // squashfs は読み取り専用の形式 (fs/squashfs)。rw を要求されても読み取り専用でマウントする (writable() == false)。非対応の圧縮方式 (LZMA1) は EOPNOTSUPP(95)
    return usbmem::SquashfsVolume::mount(dev, err);
  }
  if (t == usbmem::FsType::Erofs) {
    // erofs も読み取り専用の形式 (fs/erofs)。rw を要求されても読み取り専用でマウントする。圧縮 (z_erofs) は未対応で EOPNOTSUPP(95) (docs/erofs-port.md)
    return usbmem::ErofsVolume::mount(dev, err);
  }
  if (t == usbmem::FsType::Btrfs) {
    // Btrfs は自前の読み取り専用リーダー (docs/btrfs-port.md)。rw を要求されても読み取り専用でマウントする。tree-log が未再生なら EUCLEAN(117)。
    // 複数デバイス / RAID0・5・6・10 / 未知の機能は EOPNOTSUPP(95)。allow_noload は、ユーザーが確認したうえで tree-log を無視して開く場合だけ true (ext4 と同じ)
    usbmem::BtrfsMountOptions ob; ob.allow_noload = allow_noload;
    return usbmem::BtrfsVolume::mount(dev, err, ob);
  }
  if (t == usbmem::FsType::F2fs) {
    // f2fs は読み取り専用 (自前実装、docs/f2fs-port.md)。rw を要求されても読み取り専用。暗号化 / casefold / fs-verity などは EOPNOTSUPP(95)、
    // fsync 後のロールフォワードが必要な状態は EUCLEAN(117)。allow_noload は、確認のうえ「直近の fsync の分を無視して」開く場合だけ true
    usbmem::F2fsMountOptions of; of.allow_noload = allow_noload;
    return usbmem::F2fsVolume::mount(dev, err, of);
  }
  if (t == usbmem::FsType::Xfs) {
    // XFS は読み取り専用 (独立実装のリーダ。ログは再生しない)。rw を要求されても読み取り専用でマウントする。ログが未再生 (要リカバリ) のボリュームは EUCLEAN(117) で拒否、
    // リアルタイム領域・外部ログ・未知の incompat 機能は EOPNOTSUPP(95)。allow_noload は、ユーザーが確認したうえで読み取り専用で開く場合だけ true (docs/xfs-port.md)
    usbmem::XfsMountOptions ox; ox.allow_noload = allow_noload;
    return usbmem::XfsVolume::mount(dev, err, ox);
  }
  return rw ? usbmem::NtfsVolume::mount_rw(dev, err) : usbmem::NtfsVolume::mount(dev, err);
}

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
  auto v = mount_auto(dev, false, &err);
  if (!v) return err > 0 ? -static_cast<jlong>(err) : -5;
  return reinterpret_cast<jlong>(new Vol{std::move(v)});
}

// ext3/ext4 のジャーナル未再生 (EUCLEAN) を承知で、読み取り専用で開く (ジャーナルは再生しない。内容が古い/欠けている可能性)。fd の所有権を引き取る。ext4 以外は nativeNtfsMount と同じ
JNIEXPORT jlong JNICALL Java_org_usbmem_app_NativeBridge_nativeNtfsMountNoload(JNIEnv*, jclass, jint fd) {
  usbmem::BlockDevicePtr dev;
  usbmem::Status st = usbmem::RawImageDevice::open_fd(fd, true, true, 512, &dev);
  if (!st.ok()) return -22;
  int err = 0;
  auto v = mount_auto(dev, false, &err, true);
  if (!v) return err > 0 ? -static_cast<jlong>(err) : -5;
  return reinterpret_cast<jlong>(new Vol{std::move(v)});
}

// 書き込み可能でマウント (実験的。ビルドで USBMEM_NTFS_WRITE が無効なら -EROFS)。fd の所有権を引き取る。戻り値: >0 ハンドル / <=0 = -errno
JNIEXPORT jlong JNICALL Java_org_usbmem_app_NativeBridge_nativeNtfsMountRw(JNIEnv*, jclass, jint fd) {
  usbmem::BlockDevicePtr dev;
  usbmem::Status st = usbmem::RawImageDevice::open_fd(fd, false, true, 512, &dev);
  if (!st.ok()) return -22;
  int err = 0;
  auto v = mount_auto(dev, true, &err);
  if (!v) return err > 0 ? -static_cast<jlong>(err) : -5;
  return reinterpret_cast<jlong>(new Vol{std::move(v)});
}

// マウントしたファイルシステムの名前 ("ntfs" / "exfat" / "ext2" / "fat")
JNIEXPORT jbyteArray JNICALL Java_org_usbmem_app_NativeBridge_nativeFsName(JNIEnv* env, jclass, jlong h) {
  return bytes(env, reinterpret_cast<Vol*>(h)->v->fs_name());
}

// 上流の判断で読み取り専用に落ちていないか (1 = 書き込み可)
JNIEXPORT jboolean JNICALL Java_org_usbmem_app_NativeBridge_nativeNtfsWritable(JNIEnv*, jclass, jlong h) {
  return reinterpret_cast<Vol*>(h)->v->writable() ? JNI_TRUE : JNI_FALSE;
}

// マウントにかかった時間と、そのうち下位デバイス (USB) の読み取りに使った時間・回数・バイト数を logcat に出す (どこで時間を使っているかの診断用)
static void log_mount_stats(const char* what, const usbmem::BlockDevicePtr& ra, std::chrono::steady_clock::time_point t0, bool ok) {
  long total = (long)std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
  uint64_t reads = 0, bytes = 0, ns = 0;
  if (usbmem::ReadAheadDevice::stats(ra, &reads, &bytes, &ns))
    __android_log_print(ANDROID_LOG_INFO, "usbmem", "%s %s: total %ld ms, device reads %llu (%llu MiB) in %lld ms (%.1f MB/s), other %lld ms", what, ok ? "ok" : "FAILED", total,
                        (unsigned long long)reads, (unsigned long long)(bytes >> 20), (long long)(ns / 1000000), ns ? (double)bytes / ((double)ns / 1e9) / 1e6 : 0.0, total - (long long)(ns / 1000000));
  else
    __android_log_print(ANDROID_LOG_INFO, "usbmem", "%s %s: total %ld ms", what, ok ? "ok" : "FAILED", total);
}

// nativeNtfsMountDevice のジャーナル未再生を承知で開く版 (上の nativeNtfsMountNoload と同じ意味)
JNIEXPORT jlong JNICALL Java_org_usbmem_app_NativeBridge_nativeNtfsMountDeviceNoload(JNIEnv*, jclass, jlong devHandle, jlong offset, jlong length) {
  auto* d = reinterpret_cast<usbmem_device*>(devHandle);
  if (!d || offset < 0 || length <= 0) return -22;
  usbmem::BlockDevicePtr sub;
  usbmem::Status st = usbmem::SubRangeDevice::create(d->dev, static_cast<uint64_t>(offset), static_cast<uint64_t>(length), true, &sub);
  if (!st.ok()) return -22;
  int err = 0;
  auto ra = usbmem::ReadAheadDevice::wrap(sub);
  auto v = mount_auto(ra, false, &err, true);
  if (!v) return err > 0 ? -static_cast<jlong>(err) : -5;
  return reinterpret_cast<jlong>(new Vol{std::move(v), d->lineage});
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
  auto ra = usbmem::ReadAheadDevice::wrap(sub);
  auto t0 = std::chrono::steady_clock::now();
  auto v = mount_auto(ra, false, &err);
  log_mount_stats("mount", ra, t0, v != nullptr);
  if (!v) return err > 0 ? -static_cast<jlong>(err) : -5;
  return reinterpret_cast<jlong>(new Vol{std::move(v), d->lineage});
}

// 開いているデバイスの一区間を書き込み可能で NTFS マウント (実験的。ビルドで無効なら -EROFS)。デバイス自体も read_only=false で開かれていること。
JNIEXPORT jlong JNICALL Java_org_usbmem_app_NativeBridge_nativeNtfsMountDeviceRw(JNIEnv*, jclass, jlong devHandle, jlong offset, jlong length) {
  auto* d = reinterpret_cast<usbmem_device*>(devHandle);
  if (!d || offset < 0 || length <= 0) return -22;
  usbmem::BlockDevicePtr sub;
  usbmem::Status st = usbmem::SubRangeDevice::create(d->dev, static_cast<uint64_t>(offset), static_cast<uint64_t>(length), false, &sub);
  if (!st.ok()) return -22;
  int err = 0;
  auto ra = usbmem::ReadAheadDevice::wrap(sub);
  auto t0 = std::chrono::steady_clock::now();
  auto v = mount_auto(ra, true, &err);
  log_mount_stats("mount_rw", ra, t0, v != nullptr);
  if (!v) return err > 0 ? -static_cast<jlong>(err) : -5;
  return reinterpret_cast<jlong>(new Vol{std::move(v), d->lineage});
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
  std::vector<usbmem::FsDirEntry> es;
  if (!rc) rc = v->readdir(ino, &es);
  if (rc) { jint e = rc < 0 ? -rc : rc; env->SetIntArrayRegion(err, 0, 1, &e); return nullptr; }
  std::string o = "[";
  bool first = true;
  for (auto& e : es) {
    if (e.name == "." || e.name == "..") continue;
    usbmem::FsStat st;
    if (v->stat(e.ino, &st)) continue;
    if (!first) o += ',';
    first = false;
    o += "{\"name\":"; json_str(o, e.name);
    o += ",\"ino\":\"" + std::to_string(e.ino) + "\",\"dir\":" + (st.is_dir() ? "true" : "false");
    o += std::string(",\"link\":") + (((st.mode & 0170000) == 0120000) ? "true" : "false");
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
  usbmem::FsStat st;
  int rc = v->resolve(utf8(env, path), &ino);
  if (!rc) rc = v->stat(ino, &st);
  if (rc) { jint e = rc < 0 ? -rc : rc; env->SetIntArrayRegion(err, 0, 1, &e); return nullptr; }
  std::string o = "{\"ino\":\"" + std::to_string(ino) + "\",\"dir\":" + (st.is_dir() ? "true" : "false") +
                  ",\"link\":" + (((st.mode & 0170000) == 0120000) ? "true" : "false") +
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
  usbmem::FsStat st;
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

// path を newParent ディレクトリの下へ newName で移動 (同じ名前でも別フォルダでも可)。0 / <0 = -errno
JNIEXPORT jlong JNICALL Java_org_usbmem_app_NativeBridge_nativeNtfsMove(JNIEnv* env, jclass, jlong h, jbyteArray path, jbyteArray newParent, jbyteArray newName) {
  auto* v = reinterpret_cast<Vol*>(h)->v.get();
  std::string dir, name;
  split_path(utf8(env, path), &dir, &name);
  uint64_t od = 0, nd = 0;
  int rc = v->resolve(dir, &od);
  if (!rc) rc = v->resolve(utf8(env, newParent), &nd);
  if (!rc) rc = v->rename(od, name, nd, utf8(env, newName));
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

// path のシンボリックリンクのリンク先を返す (読み取り専用でも可)。失敗時 null (err[0] = errno)。ext2 以外は -EOPNOTSUPP (95)
JNIEXPORT jbyteArray JNICALL Java_org_usbmem_app_NativeBridge_nativeFsReadlink(JNIEnv* env, jclass, jlong h, jbyteArray path, jintArray err) {
  auto* v = reinterpret_cast<Vol*>(h)->v.get();
  uint64_t ino = 0;
  std::string target;
  int rc = v->resolve(utf8(env, path), &ino);
  if (!rc) rc = v->readlink(ino, &target);
  if (rc) { jint e = rc < 0 ? -rc : rc; env->SetIntArrayRegion(err, 0, 1, &e); return nullptr; }
  return bytes(env, target);
}

// parent の下に name のシンボリックリンク (リンク先 target) を作る。戻り値: ino / <0 = -errno
JNIEXPORT jlong JNICALL Java_org_usbmem_app_NativeBridge_nativeFsSymlink(JNIEnv* env, jclass, jlong h, jbyteArray parent, jbyteArray name, jbyteArray target) {
  auto* v = reinterpret_cast<Vol*>(h)->v.get();
  uint64_t dir = 0, ino = 0;
  int rc = v->resolve(utf8(env, parent), &dir);
  if (!rc) rc = v->symlink(dir, utf8(env, name), utf8(env, target), &ino);
  return rc ? neg(rc) : static_cast<jlong>(ino);
}

// parent の下に、既存ファイル existing へのハードリンク name を作る。0 / <0 = -errno
JNIEXPORT jlong JNICALL Java_org_usbmem_app_NativeBridge_nativeFsLink(JNIEnv* env, jclass, jlong h, jbyteArray parent, jbyteArray name, jbyteArray existing) {
  auto* v = reinterpret_cast<Vol*>(h)->v.get();
  uint64_t dir = 0, ino = 0;
  int rc = v->resolve(utf8(env, parent), &dir);
  if (!rc) rc = v->resolve(utf8(env, existing), &ino);
  if (!rc) rc = v->link(dir, utf8(env, name), ino);
  return rc ? neg(rc) : 0;
}

// 変更をメディアへ確定 (書き込み + デバイス flush)。0 / <0 = -errno
JNIEXPORT jlong JNICALL Java_org_usbmem_app_NativeBridge_nativeNtfsSync(JNIEnv*, jclass, jlong h) {
  int rc = reinterpret_cast<Vol*>(h)->v->sync();
  return rc ? neg(rc) : 0;
}

}  // extern "C"
