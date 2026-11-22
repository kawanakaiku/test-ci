// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.kfsd

/** kfsd を fork + exec で起こす小さな JNI (android/kfsd-spawn/kfsd_spawn.cpp。libkfsdspawn.so)。個人版の :kfsd サービスと別 APK のプラグインが共有する。 */
object KfsdSpawnNative {
    init { System.loadLibrary("kfsdspawn") }

    /** `exe --fd=3 [extra...]` を起こす。[sockFd] は子の fd 3 へ複製される (呼び出し側の fd はそのまま)。[logOut] [0] に子の標準出力・標準エラーの読み側 fd。pid、失敗は -errno。 */
    @JvmStatic external fun nativeSpawn(exe: String, sockFd: Int, extra: Array<String>, logOut: IntArray): Int

    /** pid の終了を待つ (ブロック)。終了コード (シグナル死は 128 + シグナル)、待てなければ -1。 */
    @JvmStatic external fun nativeWait(pid: Int): Int
}
