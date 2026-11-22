// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.ui

import android.net.Uri
import org.json.JSONObject
import org.usbmem.app.FsCheckResult
import org.usbmem.app.MkfsApplyResult
import org.usbmem.app.MkfsPlan
import org.usbmem.app.PartitionApplyResult
import org.usbmem.app.PartitionPlan
import org.usbmem.app.PartitionTable

/*
 * ディスクツール (検査 / フォーマット / パーティション編集 / バックアップ・復元 / 救出) の画面と処理の境界。docs/disk-ui.md
 *
 * 画面 (ui/Disk*.kt) はこのインタフェースだけを呼ぶ。実処理は DiskToolsController (DiskTools / PartitionEditor / RecoveryBackup を呼ぶ)。
 * どの処理も呼び出したスレッドをふさぐので、画面は Dispatchers.IO から呼ぶ。失敗は例外ではなく [Outcome.error] (利用者に見せる文言) で返る。
 */

enum class DiskTargetKind { Usb, Image }

/** ディスクツールの対象。USB は開いているセッション (スロット名)、イメージは SAF で選んだファイル。 */
data class DiskTarget(
    val kind: DiskTargetKind,
    /** USB: スロット名 (UsbDeviceState.key) / イメージ: URI の文字列 */
    val key: String,
    /** 画面に出す名前 (機種名 + 識別子 / ファイル名) */
    val title: String,
    /** イメージだけ: イメージ画面の「書き込みを許可」が入っていたか (USB は開いているセッションの書き込みモードが決める) */
    val imageWritable: Boolean = false,
    /** 入ったときに選んでおく範囲の開始位置 (-1 = ディスク全体) */
    val preselectStart: Long = -1,
)

/** 検査・フォーマット・バックアップなどの対象範囲。[index] 0 = ディスク全体、1 以上 = パーティション表の番号。 */
data class DiskRange(val start: Long, val size: Long, val label: String, val fs: String, val index: Int, val fsLabel: String = "", val fsUuid: String = "") {
    val whole: Boolean get() = index == 0
}

/** 処理の結果。[error] が非 null なら失敗 (利用者に見せる文言)。 */
data class Outcome<T>(val value: T? = null, val error: String? = null)

/** 長い処理の進捗。[done] / [total] は処理ごとの単位 (バイト・ブロック・MFT レコードなど)。[json] は救出の詳細 (finished / bad / nonTried ...)。 */
data class OpProgress(val phase: String, val done: Long, val total: Long, val json: JSONObject? = null)

/** 長い処理の取り消し・一時停止・進捗。処理が終わってから [close]。別スレッドから呼んでよい。 */
interface DiskOpJob : AutoCloseable {
    fun cancel()
    fun pause(paused: Boolean)
    fun progress(): OpProgress?
}

/** フォーマットの指定。[seed] != 0 と [timestampSec] != 0 は、同じ指定で開き直したとき同じ計画になるよう、画面が最初の計画で決めて持ち回る。 */
data class FormatSpec(val fs: String, val label: String, val clusterSize: Int, val seed: Long, val timestampSec: Long)

/** パーティション編集の 1 操作。画面が積んで、計画と適用のたびに、同じ順序で新しいセッションへ流す (GUID は画面が決める: 計画と適用で同じ計画になるように)。 */
sealed interface PartOp {
    data class NewTable(val gpt: Boolean, val diskId: String, val overwriteFs: Boolean) : PartOp
    data class Create(val sizeBytes: Long, val name: String, val gptType: String, val mbrType: Int, val uniqueGuid: String) : PartOp
    data class Remove(val index: Int) : PartOp
    data class Resize(val index: Int, val sizeBytes: Long) : PartOp
    data class Rename(val index: Int, val name: String) : PartOp
    data class SetType(val index: Int, val gptType: String, val mbrType: Int) : PartOp
    data class Repair(val kind: String) : PartOp
    /** パーティション表のバックアップファイル (SAF) から戻す。ほかの操作とは組み合わせない */
    data class RestoreTable(val uri: Uri, val fileName: String) : PartOp
}

/** 救出の指定 (UI で選べる分)。 */
data class RescueSpec(
    val retries: Int, val noTrim: Boolean, val noScrape: Boolean, val pauseBetweenReadsMs: Int,
    val maxConsecutiveErrors: Int, val noProgressTimeoutSec: Int, val clusterSectors: Int,
)

interface DiskToolsActions {
    /** [t] の今の書き込みモード・マウント状態 (USB は現在のセッション、イメージは [DiskTarget.imageWritable])。破壊的な操作の前に必ず見る */
    fun writeAllowed(t: DiskTarget): Boolean
    /** [r] の範囲を、このアプリ (USB はこのデバイスのマウント / イメージはマウント中のイメージ) か OS が使っているか */
    fun inUse(t: DiskTarget, r: DiskRange): Boolean
    fun readTable(t: DiskTarget): Outcome<PartitionTable>
    fun newCheckJob(): DiskOpJob
    fun newRecoveryJob(): DiskOpJob

    // ---- 検査 (読み取り専用)
    fun check(t: DiskTarget, r: DiskRange, job: DiskOpJob): Outcome<FsCheckResult>

    // ---- フォーマット
    /** 作れるファイルシステムと最小サイズ (ネイティブへの問い合わせ。名前は小文字: ext2 / exfat / fat32 ...) */
    fun formatChoices(): List<Pair<String, Long>>
    /** [fs] を [r] に作るときに使えるクラスタ / ブロックサイズ (バイト)。0 = 自動を先頭に、計画が通るものだけ (ネイティブの計画で確かめる) */
    fun formatClusterChoices(t: DiskTarget, r: DiskRange, fs: String): List<Int>
    fun formatPlan(t: DiskTarget, r: DiskRange, spec: FormatSpec): Outcome<MkfsPlan>
    /** 計画を作り直して [expectedPhrase] と一致することを確かめてから、[typed] で適用する (書き込みモード・マウント状態もここで確かめる) */
    fun formatApply(t: DiskTarget, r: DiskRange, spec: FormatSpec, expectedPhrase: String, typed: String, job: DiskOpJob): Outcome<FormatDone>

    // ---- パーティション編集
    fun partitionGpt(t: DiskTarget): Outcome<JSONObject>
    fun partitionPlan(t: DiskTarget, ops: List<PartOp>): Outcome<PartitionPlan>
    fun partitionApply(t: DiskTarget, ops: List<PartOp>, expectedPhrase: String, typed: String): Outcome<PartitionDone>
    /** 今のパーティション表のバックアップを [dest] (SAF) へ保存する。戻り値 = バイト数 */
    fun partitionBackupTo(t: DiskTarget, dest: Uri): Outcome<Int>

    // ---- バックアップ / 復元 (Partclone 形式)
    fun backupRun(t: DiskTarget, r: DiskRange, dest: Uri, lz4: Boolean, fullIfUnclean: Boolean, job: DiskOpJob): Outcome<JSONObject>
    /** 書いた画像の全体のチェックサムを確かめる (読み取りだけ) */
    fun imageInspect(src: Uri, verify: Boolean): Outcome<JSONObject>
    fun restorePlan(t: DiskTarget, r: DiskRange, src: Uri): Outcome<JSONObject>
    fun restoreApply(t: DiskTarget, r: DiskRange, src: Uri, expectedPhrase: String, typed: String, job: DiskOpJob): Outcome<JSONObject>

    // ---- 救出 (ddrescue 相当)
    /** [t] の [r] (読み取り元) を [out] (出力イメージ) へ複製する前の検査 (rescue_preflight)。結果 JSON に警告 ("warnings") も含む */
    fun rescuePreflight(t: DiskTarget, r: DiskRange, out: Uri, spec: RescueSpec): Outcome<JSONObject>
    fun rescueRun(t: DiskTarget, r: DiskRange, out: Uri, spec: RescueSpec, job: DiskOpJob): Outcome<JSONObject>
    /** 救出の進捗ファイル (mapfile) を捨てて最初からやり直す */
    fun rescueResetMap(t: DiskTarget, r: DiskRange, out: Uri)
}

/** フォーマットの結果。[backupSaved] = 書く前に上書きする領域の元の内容をアプリ内に保存した (名前だけ。場所は画面に出さない) */
data class FormatDone(val result: MkfsApplyResult, val backupName: String?)

/** パーティション編集の結果。[after] = 適用後に読み直した表 */
data class PartitionDone(val result: PartitionApplyResult, val backupName: String?, val after: PartitionTable?)
