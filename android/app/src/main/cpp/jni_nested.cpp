// SPDX-License-Identifier: GPL-2.0-or-later
// ネスト (ループバック) マウントの JNI。マウント済みボリューム (Vol ハンドル) の中のファイルを、読み取り専用の BlockDevice (usbmem_device ハンドル) として開く。
// 開いたデバイスは、既存の nativeListPartitions (パーティション一覧) / nativeNtfsMountDevice (一区間のマウント) / nativeClose にそのまま渡せる。docs/nested-mount.md
#include <jni.h>
#include <android/log.h>

#include <cerrno>
#include <string>

#include "jni_vol.h"
#include "usbmem/c_api_internal.h"
#include "usbmem/nested_mount.h"

extern "C" {

// volHandle のボリュームの path にあるファイルをデバイスとして開く。戻り値: >0 デバイスハンドル / <=0 = -errno
//  ELOOP(40) = 段数の上限 (3) を超える / 循環、EISDIR(21) = ディレクトリ、EINVAL(22) = 通常ファイルでない・小さすぎる。
//  閉じるときは nativeClose。そのデバイスの上に載せたボリュームを先に nativeNtfsUnmount すること (子 → 親の順)。
JNIEXPORT jlong JNICALL Java_org_usbmem_app_NativeBridge_nativeNestedOpen(JNIEnv* env, jclass, jlong volHandle, jbyteArray path) {
  auto* p = reinterpret_cast<Vol*>(volHandle);
  if (!p || !path) return -EINVAL;
  jsize n = env->GetArrayLength(path);
  std::string s(static_cast<size_t>(n), '\0');
  if (n) env->GetByteArrayRegion(path, 0, n, reinterpret_cast<jbyte*>(&s[0]));
  usbmem::NestedOpen o = usbmem::open_nested_image(p->v, p->lineage, s);
  if (o.err) {
    __android_log_print(ANDROID_LOG_INFO, "usbmem", "nested open refused: errno %d (%s)", o.err, o.why.c_str());
    return -static_cast<jlong>(o.err);
  }
  auto* d = new usbmem_device{o.dev, o.lineage};
  return reinterpret_cast<jlong>(d);
}

// ボリュームのネスト段数 (最上位 = 0、イメージの中のボリューム = 1、…)
JNIEXPORT jint JNICALL Java_org_usbmem_app_NativeBridge_nativeNestedDepth(JNIEnv*, jclass, jlong volHandle) {
  auto* p = reinterpret_cast<Vol*>(volHandle);
  return p && p->lineage ? p->lineage->depth : 0;
}

}  // extern "C"
