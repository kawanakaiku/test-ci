package org.usbmem.app

/**
 * 暗号ボリューム (LUKS1/2・BitLocker) を解錠して、中のファイルシステムをマウントする層 (UI と [CryptoVolume] の間)。docs/crypto-volumes.md 9 章・10 章
 *
 * 流れ: 暗号パーティションの範囲を [NativeBridge.nativeDeviceSubRangeMode] で切り出す → [CryptoVolume.probe] / [CryptoVolume.openWithInfo] で解錠 →
 * 復号済みデバイスの中のパーティション表を読み (表が無ければ全体が 1 つの FS)、**中のパーティションの開始位置・長さ**で各 FS をマウントする
 * (開始位置 0 固定ではない: 解錠した中に GPT があるとき、FS はパーティションの途中から始まる)。
 *
 * 資格情報は ByteArray だけで扱う。[UnlockCredentials] は使い終えたらゼロ埋めする (CryptoVolume.open も呼び出しの最後に埋める)。ログ・例外のメッセージ・
 * SharedPreferences に資格情報を出さない。
 */
class UnlockCredentials(var password: ByteArray? = null, var recoveryKey: ByteArray? = null, var keyFile: ByteArray? = null) {
    /** 保持している秘密をすべてゼロで埋める (呼び出し後に使い回さない)。 */
    fun wipe() {
        password?.fill(0); recoveryKey?.fill(0); keyFile?.fill(0)
        password = null; recoveryKey = null; keyFile = null
    }

    val isEmpty: Boolean get() = (password?.isEmpty() ?: true) && (recoveryKey?.isEmpty() ?: true) && (keyFile?.isEmpty() ?: true)
}

/**
 * 1 つの暗号ボリュームの解錠で作った、下位のハンドル (復号済みデバイス・パーティションの切り出し・必要なら元のイメージ) の後始末。
 * 中の FS を複数マウントしたときは、全部が [release] するまで閉じない。最後の 1 つが閉じたとき、復号済みデバイス → 切り出し → 元の順に閉じる
 * (復号器が保持する鍵とキャッシュは復号済みデバイスを閉じるときにゼロ化される)。
 */
class CryptoGroup(val id: String, private val closers: List<() -> Unit>) {
    private var refs = 0
    private var closed = false   // 下位のハンドルは 1 度だけ閉じる (release と closeNow が両方呼ばれても二重に閉じない)
    @Synchronized fun retain() { refs++ }
    @Synchronized fun release() {
        refs--
        if (refs == 0) closeAll()
    }
    /** FS を 1 つもマウントできなかったとき (retain していない) の後始末。 */
    @Synchronized fun closeNow() { if (refs == 0) closeAll() }
    private fun closeAll() { if (!closed) { closed = true; closers.forEach { runCatching { it() } } } }
}

/**
 * [CryptoMount.unlock] が中の FS を開かずに保留したものの範囲 (復号済みデバイス上の位置)。
 * [dirty] = false: ジャーナル未再生 (EUCLEAN=117) で、読み取り専用 (noload) でしか開けない。
 * [dirty] = true: ダーティフラグだけの NTFS に書き込み可を要求した ([WriteConfirmRequired]。何も書いていない)。確認すれば、読み取り専用か書き込み可で開ける。
 */
class InnerRange(val start: Long, val size: Long, val suffix: String, val dirty: Boolean = false)

/**
 * 解錠は成功したが、中の ext3/ext4 (将来は Btrfs / F2FS / XFS) がジャーナル未再生 (EUCLEAN) で開けなかった FS を、**ユーザーの確認が出るまで保留**するもの。
 * 保持するのは復号済みデバイスのハンドル ([group] が持ち主。この保留が [CryptoGroup.retain] している) だけで、**資格情報 (パスワード等) は保持しない**
 * ([CryptoMount.unlock] が解錠の直後にゼロ埋め済み)。確認 → [mountNoload] で読み取り専用 (noload) にマウント、キャンセル・破棄 → [release] で鍵ごと閉じる。
 * 保留は [VolumeRegistry] が登録簿に持ち、USB を閉じる・アプリの終了・同じボリュームの解錠のやり直しで [release] される。
 */
class PendingRecovery internal constructor(val group: CryptoGroup, private val dec: Long, val ranges: List<InnerRange>) {
    private var released = false
    /** ダーティフラグだけの NTFS (読み取り専用か書き込み可かを選べる) を含むか */
    val hasDirty: Boolean get() = ranges.any { it.dirty }
    /** ジャーナル未再生の FS (読み取り専用でしか開けない) を含むか */
    val hasJournal: Boolean get() = ranges.any { !it.dirty }

    class Mounted(val volumes: List<UnlockedCrypto.Inner>, val skipped: List<String>)

    /**
     * 保留していた FS を、ジャーナルを再生せずに**読み取り専用**でマウントする (書き込みは一切しない)。1 つも開けなければ例外 (この場合は保留も [release] 済み)。
     * 成功したときは保留の参照が残る: 呼び出し側が [VolumeRegistry.addCryptoVolume] で登録 (= retain) したあとに [release] すること。
     */
    fun mountNoload(): Mounted = mountPending(writeDirty = false, createOnly = true)

    /**
     * [mountNoload] の、ダーティフラグだけの NTFS を書き込み可で開けるもの: [writeDirty] = true なら、そのような範囲は書き込み可 ([createOnly] = 新規作成のみ) で開く
     * (確認のうえ。ダーティフラグを承知)。ジャーナル未再生の FS は常に読み取り専用 (noload)。
     */
    @Synchronized
    fun mountPending(writeDirty: Boolean, createOnly: Boolean): Mounted {
        check(!released) { "recovery already released" }
        val mounted = ArrayList<UnlockedCrypto.Inner>()
        val skipped = ArrayList<String>()
        var firstError: NativeException? = null
        for (r in ranges) {
            try {
                val v = if (r.dirty && writeDirty) NtfsVolume.mountDeviceRangeReadWrite(dec, r.start, r.size, createOnly, allowDirty = true)
                else NtfsVolume.mountDeviceRange(dec, r.start, r.size, allowNoload = true)
                mounted += UnlockedCrypto.Inner(v, r.suffix, r.start)
            } catch (e: NativeException) {
                if (firstError == null) firstError = e
                skipped += "${r.suffix.ifEmpty { "-" }}: errno ${e.code}"
            }
        }
        if (mounted.isEmpty()) { releaseLocked(); throw firstError ?: NativeException(95, "no mountable filesystem inside the encrypted volume") }
        return Mounted(mounted.toList(), skipped)
    }

    /** 保留を手放す (1 回だけ有効)。他に参照が無ければ、復号済みデバイス → 切り出し → 元の順に閉じ、鍵をゼロ化する。 */
    @Synchronized fun release() = releaseLocked()

    private fun releaseLocked() { if (!released) { released = true; group.release() } }
}

/** [VolumeRegistry.Entry] に付く、暗号ボリュームの情報 (画面と SAF のルートの説明用。秘密は含まない)。[groupId] = 同じ解錠から出たボリュームの組。 */
data class CryptoMeta(val format: String, val cipher: String, val writable: Boolean, val groupId: String)

/** [CryptoMount.unlock] の結果。[volumes] = 中でマウントできた FS (label は中のパーティションの接尾辞。全体が 1 つなら空)。 */
class UnlockedCrypto(
    val group: CryptoGroup,
    val info: CryptoInfo?,
    val volumes: List<Inner>,
    /** 書き込み可を要求したのに、読み取り専用で開いた理由 (人に見せてよい)。要求していない・書き込み可で開けたときは空。 */
    val writeNote: String,
    /** 書き込み可を要求したか */
    val writeRequested: Boolean,
    /** マウントしなかった中のパーティションの理由 (人に見せてよい。秘密を含まない) */
    val skipped: List<String>,
    /** ジャーナル未再生 / ダーティな NTFS で保留した FS (確認のうえ開く)。無ければ null。非 null のとき、呼び出し側は必ず [PendingRecovery.release] か [PendingRecovery.mountNoload] の後の release をすること */
    val recovery: PendingRecovery? = null,
) {
    class Inner(val volume: NtfsVolume, val suffix: String, val startBytes: Long)
    val writable: Boolean get() = volumes.any { it.volume.writable }
}

object CryptoMount {
    /** 解錠に使う方式 (人が選んだもの)。 */
    enum class Method { Password, RecoveryKey, KeyFile }

    private val INNER_MOUNTABLE = setOf("ntfs", "exfat", "ext2", "ext3", "ext4", "fat12", "fat16", "fat32", "iso9660", "udf", "squashfs", "erofs", "btrfs", "f2fs", "xfs")

    /** パーティション表の fs 名が暗号ボリュームか ("luks" / "bitlocker")。 */
    fun isCryptoFs(fs: String): Boolean = fs == "luks" || fs == "bitlocker"

    /** 暗号ボリュームの説明 (UI に出す書式名)。 */
    fun displayFormat(info: CryptoInfo): String = info.format.ifBlank { if (info.kind == CryptoKind.BitLocker) "BitLocker" else "LUKS" }

    /**
     * [parent] の [start, start+size) が暗号ボリュームなら、その種類と使える解錠方法を返す (資格情報は使わない。何もマウントしない)。暗号でなければ null。
     * 失敗は [NativeException] / [CryptoException]。
     */
    fun probe(parent: Long, start: Long, size: Long): CryptoInfo? {
        val sub = NativeBridge.nativeDeviceSubRangeMode(parent, start, size, true)
        if (!Handle.valid(sub)) throw NativeException((-sub).toInt(), "sub range failed: ${NativeBridge.nativeErrcName((-sub).toInt())}")
        try { return CryptoVolume.probe(sub) } finally { NativeBridge.nativeClose(sub) }
    }

    /**
     * 解錠して、中の FS をマウントする。[cred] は呼び出しの最後にゼロ埋めされる (成功・失敗とも)。
     *
     * @param parentWritable [parent] が書き込み可で開かれているか。false なら、[wantWrite] でも読み取り専用 ([UnlockedCrypto.writeNote] に理由)。
     * @param wantWrite 書き込み可で開く要求。core が拒否したとき (非対応の方式・メタデータの食い違い・FS が読み取り専用に落とした等) は、解錠は成功のまま
     *   読み取り専用で開き、理由を [UnlockedCrypto.writeNote] に入れる。
     * @param createOnly 書き込み可のとき、新規作成だけを許す (USB の「新規作成のみ」)。false ならすべての操作。
     * @param groupId [CryptoGroup] / [CryptoMeta] の識別子
     * @param extraClose 復号済みデバイスと切り出しを閉じた後に呼ぶ後始末 (元のイメージのハンドルを閉じるなど)。**失敗したときも呼ばれる**
     *   (この関数が例外を投げたら、呼び出し側は何も閉じなくてよい)。成功したときは [UnlockedCrypto.group] が最後の FS を閉じたときに呼ぶ。
     * 失敗は [CryptoException] (認証失敗は authFailed) か [NativeException] (中の FS のマウント失敗など)。
     *
     * 中の FS が**ジャーナル未再生 (EUCLEAN=117)**、または書き込み可を要求した**ダーティフラグだけの NTFS** のときは失敗にせず、その範囲を [UnlockedCrypto.recovery] に保留して返す (ほかの FS は普通にマウントする。
     * 1 つも開けた FS が無くても例外にしない)。呼び出し側はユーザーに「読み取り専用で開く」確認を取り、[PendingRecovery.mountNoload] か [PendingRecovery.release] をすること。
     * 書き込み可を要求していても、ジャーナル未再生の FS は書き込み可では開かない (noload は読み取り専用のみ)。ダーティフラグだけの NTFS は、確認のうえ [PendingRecovery.mountPending] で書き込み可にできる。
     */
    fun unlock(
        parent: Long, parentWritable: Boolean, start: Long, size: Long, cred: UnlockCredentials, method: Method,
        wantWrite: Boolean, createOnly: Boolean, groupId: String, extraClose: (() -> Unit)? = null,
    ): UnlockedCrypto {
        val wantRw = wantWrite && parentWritable
        // 選んだ方式の資格情報だけを渡す (他は渡さない。全部、最後の finally でゼロ埋め)
        val pw = if (method == Method.Password) cred.password else null
        val rk = if (method == Method.RecoveryKey) cred.recoveryKey else null
        val kf = if (method == Method.KeyFile) cred.keyFile else null
        var sub = 0L
        var dec = 0L
        val mounted = ArrayList<UnlockedCrypto.Inner>()
        try {
            val s = NativeBridge.nativeDeviceSubRangeMode(parent, start, size, !wantRw)
            if (!Handle.valid(s)) throw NativeException((-s).toInt(), "sub range failed: ${NativeBridge.nativeErrcName((-s).toInt())}")
            sub = s
            val info0 = CryptoVolume.probe(sub) ?: throw CryptoException(CryptoException.UNSUPPORTED, null, "not an encrypted volume")
            val opened = CryptoVolume.openWithInfo(sub, info0.kind, pw, rk, kf, writable = wantRw)
            dec = opened.handle
            val info = opened.info ?: info0
            val coreRw = wantRw && info.writable
            var note = when {
                !wantWrite -> ""
                !parentWritable -> "このデバイスは読み取り専用で開いています"
                !info.writable -> info.writeNote.ifBlank { "この暗号ボリュームは書き込みに対応していません" }
                else -> ""
            }
            val skipped = ArrayList<String>()
            val recoveryRanges = ArrayList<InnerRange>()
            var firstError: NativeException? = null
            for (r in innerRanges(dec)) {
                try {
                    val v = if (coreRw) NtfsVolume.mountDeviceRangeReadWrite(dec, r.start, r.size, createOnly) else NtfsVolume.mountDeviceRange(dec, r.start, r.size, false)
                    if (coreRw && !v.writable && note.isEmpty()) note = "中のファイルシステム (${v.fsName}) は書き込みに対応していない、または未処理の異常があるため、読み取り専用で開きました"
                    mounted += UnlockedCrypto.Inner(v, r.suffix, r.start)
                } catch (e: WriteConfirmRequired) {
                    recoveryRanges += InnerRange(r.start, r.size, r.suffix, dirty = true); continue   // ダーティフラグだけの NTFS: 確認すれば読み取り専用 / 書き込み可で開ける (保留。何も書いていない)
                } catch (e: NativeException) {
                    if (e.code == 117) { recoveryRanges += r; continue }   // ジャーナル未再生: 確認すれば読み取り専用で開ける (保留)
                    if (firstError == null) firstError = e
                    skipped += "${r.suffix.ifEmpty { "-" }}: errno ${e.code}"
                }
            }
            if (mounted.isEmpty() && recoveryRanges.isEmpty()) throw firstError ?: NativeException(95, "no mountable filesystem inside the encrypted volume")
            // 成功: 下位のハンドルの持ち主はグループ (最後の FS が閉じたとき、復号済みデバイス → 切り出し → 元の順に閉じる)
            val d = dec; val sb = sub
            val group = CryptoGroup(groupId, listOfNotNull<() -> Unit>({ NativeBridge.nativeClose(d) }, { NativeBridge.nativeClose(sb) }, extraClose))
            val pending = if (recoveryRanges.isEmpty()) null else PendingRecovery(group, d, recoveryRanges.toList()).also { group.retain() }
            dec = 0; sub = 0
            return UnlockedCrypto(group, info, mounted.toList(), note, wantWrite, skipped, pending)
        } catch (t: Throwable) {
            for (m in mounted) runCatching { m.volume.close() }
            if (dec != 0L) runCatching { NativeBridge.nativeClose(dec) }
            if (sub != 0L) runCatching { NativeBridge.nativeClose(sub) }
            runCatching { extraClose?.invoke() }
            throw t
        } finally {
            cred.wipe()
        }
    }

    /** 復号済みデバイス [dec] の中の、マウントする範囲。表が無く全体が 1 つの FS なら全体。表があれば、マウントできる FS の各パーティション (開始位置はパーティションのもの)。 */
    private fun innerRanges(dec: Long): List<InnerRange> {
        val err = IntArray(1)
        val bytes = NativeBridge.nativeListPartitions(dec, err)
            ?: throw NativeException(err[0], "inner partition scan failed: ${NativeBridge.nativeErrcName(err[0])}")
        val t = PartitionTable.parse(String(bytes, Charsets.UTF_8))
        if (t.wholeDeviceVolume) {
            val fs = t.partitions.firstOrNull()?.fs ?: ""
            if (fs !in INNER_MOUNTABLE) throw NativeException(95, "unsupported filesystem inside the encrypted volume: $fs")
            if (!Edition.supports(fs)) throw NativeException(Edition.KFSD_REQUIRED_CODE, "filesystem needs the kfsd plugin in this edition: $fs")
            return listOf(InnerRange(0, t.diskBytes, ""))
        }
        val cands = t.partitions.filter { !it.extendedContainer && !it.protective && it.fs in INNER_MOUNTABLE }
        val out = cands.filter { Edition.supports(it.fs) }.map { InnerRange(it.startBytes, it.sizeBytes, " #${it.index}") }
        if (out.isEmpty() && cands.isNotEmpty()) throw NativeException(Edition.KFSD_REQUIRED_CODE, "no filesystem of this edition inside the encrypted volume (kfsd plugin needed)")   // 公開版: 中の FS がこの版に入っていない
        if (out.isEmpty()) throw NativeException(95, "no supported filesystem inside the encrypted volume")
        return out
    }
}
