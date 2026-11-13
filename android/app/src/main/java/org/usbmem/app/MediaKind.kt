package org.usbmem.app

/** USB 大容量記憶装置の媒体が、回転ディスク (HDD) か、フラッシュ/SSD か。docs/media-kind.md。ネイティブの MediaKind と同じ値。 */
enum class MediaKind {
    /** 判別できなかった (USB メモリの可能性がある。[MediaHint] を見る) */
    Unknown,
    /** 回転ディスク (HDD)。回転数が分かれば [MediaKindInfo.rpm] */
    Rotational,
    /** フラッシュ / SSD (回転しない) */
    NonRotational,
}

/** 判定の根拠 (どの方法で分かったか)。 */
enum class MediaBasis { None, VpdBlockDeviceChars, AtaInfoVpd, AtaPassThrough12, AtaPassThrough16 }

/** 判定できなかったときの手掛かり (確定ではない)。 */
enum class MediaHint {
    None,
    /** RMB=1 (リムーバブル) で回転数の情報が得られない: USB メモリ (フラッシュ) の可能性が高い */
    RemovableNoVpd,
    /** UNMAP/TRIM に対応: SSD・フラッシュのヒント */
    UnmapSupported,
}

/**
 * [UsbSession.mediaKind]。デバイスを開くときに 1 度だけ判別した結果。
 * @param rpm 回転ディスクで回転数が分かるとき (0 = 不明)
 * @param probed false = 判別をしていない (Unknown のまま)
 * @param detail 診断用の生の値の要約 (ASCII の 1 行。例: "rmb=0 ver=06 vpd=00,80,b1 b1=0x1c20 -> rotational 7200rpm [vpd-b1]")
 */
data class MediaKindInfo(
    val kind: MediaKind = MediaKind.Unknown,
    val rpm: Int = 0,
    val basis: MediaBasis = MediaBasis.None,
    val hint: MediaHint = MediaHint.None,
    val removable: Boolean = false,
    val probed: Boolean = false,
    val detail: String = "",
) {
    /** 回転ディスクと分かっているときだけ true。スピンダウンは、これ以外では既定で隠す。 */
    val isRotational: Boolean get() = kind == MediaKind.Rotational

    companion object {
        val UNKNOWN = MediaKindInfo()

        /** ネイティブの [kind, rpm, basis, hint, removable, probed] から作る。範囲外の値は Unknown / None に倒す。 */
        fun fromNative(v: IntArray, detail: String?): MediaKindInfo {
            if (v.size < 6) return UNKNOWN
            val kind = MediaKind.entries.getOrNull(v[0]) ?: MediaKind.Unknown
            return MediaKindInfo(
                kind = kind,
                rpm = if (kind == MediaKind.Rotational && v[1] > 0) v[1] else 0,
                basis = MediaBasis.entries.getOrNull(v[2]) ?: MediaBasis.None,
                hint = MediaHint.entries.getOrNull(v[3]) ?: MediaHint.None,
                removable = v[4] != 0,
                probed = v[5] != 0,
                detail = detail.orEmpty(),
            )
        }
    }
}
