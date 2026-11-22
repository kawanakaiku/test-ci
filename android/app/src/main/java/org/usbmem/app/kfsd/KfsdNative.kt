// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.kfsd

/** kfsd (別プロセスの FS ドライバ。docs/kfsd.md) の本体側の JNI。android/app/src/main/cpp/jni_kfsd.cpp。 */
object KfsdNative {
    init { System.loadLibrary("usbmem_jni") }

    /** 0 = プロセス内でマウントする (従来)、1 = kfsd 経由 (RPC)。[Kfsd.apply] が設定する。 */
    @JvmStatic external fun nativeSetMode(mode: Int)
    @JvmStatic external fun nativeGetMode(): Int

    /** ネイティブのマウント処理から呼ばれる: kfsd を起こして、本体側の接続済みソケットの fd (detach 済み。所有権はネイティブへ) を返す。失敗は -errno (kfsd が無ければ -65 = ENOPKG)。 */
    @JvmStatic fun spawnChildFd(): Int = Kfsd.spawnChildFd()
}
