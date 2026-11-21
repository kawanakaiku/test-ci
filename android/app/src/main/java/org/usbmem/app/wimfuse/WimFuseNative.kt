package org.usbmem.app.wimfuse

/** 個人版の FUSE 層 (別プロセスの wimlib) の本体側の JNI。core/fuseshim/wimfuse/android/jni_wimfuse.cpp。docs/fuse-layer.md 10 章。 */
object WimFuseNative {
    init { System.loadLibrary("usbmem_jni") }

    /** fd = 子とつながったソケット (detach 済み。所有権はネイティブへ)、dev = WIM を読む device ハンドル。WIM のヘッダ・イメージ一覧の JSON。失敗は null と err[0] = errno。 */
    @JvmStatic external fun nativeInfo(fd: Int, dev: Long, err: IntArray): ByteArray?
    /** image (1 始まり) を読み取り専用でマウントし、[org.usbmem.app.NtfsVolume] と同じ形のハンドルを返す。失敗は 0 と err[0] = errno。 */
    @JvmStatic external fun nativeMount(fd: Int, dev: Long, image: Int, err: IntArray): Long
    @JvmStatic external fun nativeLastMessage(): ByteArray
}
