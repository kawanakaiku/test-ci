package org.usbmem.app.wimfuse

/** 個人版の FUSE 層 (別プロセスの wimlib) の本体側の JNI。core/fuseshim/wimfuse/android/jni_wimfuse.cpp。docs/fuse-layer.md 10 章。 */
object WimFuseNative {
    init { System.loadLibrary("usbmem_jni") }

    /** fd = 子とつながったソケット (detach 済み。所有権はネイティブへ)、dev = WIM を読む device ハンドル。WIM のヘッダ・イメージ一覧の JSON。失敗は null と err[0] = errno。 */
    @JvmStatic external fun nativeInfo(fd: Int, dev: Long, err: IntArray): ByteArray?
    /** image (1 始まり) を読み取り専用でマウントし、[org.usbmem.app.NtfsVolume] と同じ形のハンドルを返す。失敗は 0 と err[0] = errno。 */
    @JvmStatic external fun nativeMount(fd: Int, dev: Long, image: Int, err: IntArray): Long
    /** WIM 全体を、イメージごとのフォルダを並べたボリュームとして開く ([WimFuse.openArchive])。子は、フォルダを最初に開いたときに [spawnChildFd] (token) で起こす。infoOut[0] = JSON。失敗は 0。 */
    @JvmStatic external fun nativeOpenArchive(dev: Long, token: Long, infoOut: Array<ByteArray?>, err: IntArray): Long
    @JvmStatic external fun nativeLastMessage(): ByteArray

    /** ネイティブの WimVolume から呼ばれる: token の起こし方で子を起こして、接続済みソケットの fd (detach 済み) を返す。失敗は -errno。 */
    @JvmStatic fun spawnChildFd(token: Long): Int = WimFuse.spawnFor(token)
}
