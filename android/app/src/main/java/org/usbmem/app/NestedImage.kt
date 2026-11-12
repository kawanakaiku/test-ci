package org.usbmem.app

/** ネストの段数の上限 (ネイティブの kMaxNestDepth と同じ。最上位のボリュームを 0 として、イメージの中のボリュームは 1..3)。 */
const val NESTED_MAX_DEPTH = 3

/**
 * ボリューム内のディスクイメージ (ループバック) を、読み取り専用のデバイスとして開いたもの。[NtfsVolume.openImage] で得る。
 * 親ボリュームのファイルをネイティブ層で直接読む (SAF の fd は経由しない)。
 *
 * 閉じる順は 内 → 外: この上にマウントしたボリューム ([mount] の戻り値) → この [NestedImage] → 親ボリューム。
 * 通常は [VolumeRegistry] が依存関係を持って、この順に閉じる。
 */
class NestedImage internal constructor(private var handle: Long) : AutoCloseable {
    /** パーティション表 (MBR / GPT / 論理パーティション)。表が無く全体が 1 つのファイルシステムなら [PartitionTable.wholeDeviceVolume]。 */
    @Synchronized
    fun partitionTable(): PartitionTable {
        check(Handle.valid(handle)) { "closed" }
        val err = IntArray(1)
        val bytes = NativeBridge.nativeListPartitions(handle, err)
            ?: throw NativeException(err[0], "partition scan failed: ${NativeBridge.nativeErrcName(err[0])}")
        return PartitionTable.parse(String(bytes, Charsets.UTF_8))
    }

    /** [offsetBytes, offsetBytes + sizeBytes) をファイルシステムとして読み取り専用でマウントする (全体なら 0 とイメージのサイズ)。 */
    @Synchronized
    fun mount(offsetBytes: Long, sizeBytes: Long, allowNoload: Boolean = false): NtfsVolume {
        check(Handle.valid(handle)) { "closed" }
        return NtfsVolume.mountDeviceRange(handle, offsetBytes, sizeBytes, allowNoload)
    }

    @Synchronized
    override fun close() {
        if (Handle.valid(handle)) NativeBridge.nativeClose(handle)
        handle = 0
    }
}
