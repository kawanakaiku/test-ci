// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.wimfuse

import android.content.Context
import org.usbmem.app.NtfsVolume

// 公開版の WIM (個人版の FUSE 層) のスタブ (docs/editions.md 12 章)。
// 個人版の org.usbmem.app.wimfuse (別プロセスの wimlib。LGPL-3.0+ / GPL-3.0+) は公開版に入らない。
// 公開版に残る VolumeRegistry / MainActivity / NestedMountSection が参照する型だけを、同じパッケージ・同じシグネチャで置く。
// WimFuse.available() は常に false なので、WIM の判定 (VolumeRegistry.inspectWim) は null を返し、WIM の画面・マウントには入らない。
// 子プロセス・JNI (WimFuseNative) は公開版に無く、このスタブは System.loadLibrary も外部関数も使わない。

/** [WimFuse] の失敗。公開版では起きない (available() が false のため呼ばれない)。 */
class WimFuseException(val errno: Int, message: String, val childMessage: String = "") : Exception(message)

/** WIM のイメージ 1 つ分 (個人版と同じ形)。 */
data class WimImageInfo(val index: Int, val name: String, val description: String, val dirs: Long, val files: Long, val bytes: Long, val product: String, val build: String)

/** WIM のヘッダとイメージ一覧 (個人版と同じ形)。 */
data class WimInfo(val imageCount: Int, val bootIndex: Int, val compression: String, val chunkSize: Long, val totalBytes: Long, val pipable: Boolean, val split: Boolean, val images: List<WimImageInfo>)

/** 公開版には無い。型だけ。 */
class WimArchive internal constructor(val volume: NtfsVolume, val info: WimInfo) : AutoCloseable {
    override fun close() {}
}

/** 公開版には無い。型だけ。 */
class WimImage internal constructor(val volume: NtfsVolume) : AutoCloseable {
    override fun close() {}
}

object WimFuse {
    private const val ENOSYS = 38

    /** 公開版は常に false。 */
    @Suppress("UNUSED_PARAMETER")
    fun available(ctx: Context): Boolean = false

    @Suppress("UNUSED_PARAMETER")
    fun info(ctx: Context, dev: Long): WimInfo = throw unsupported()

    @Suppress("UNUSED_PARAMETER")
    fun openArchive(ctx: Context, dev: Long): WimArchive = throw unsupported()

    @Suppress("UNUSED_PARAMETER")
    fun open(ctx: Context, dev: Long, image: Int): WimImage = throw unsupported()

    private fun unsupported() = WimFuseException(ENOSYS, "WIM is not supported in the public edition")
}
