// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.ui

import android.net.Uri
import org.json.JSONObject
import org.usbmem.app.BootApplyResult
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
data class FormatSpec(
    val fs: String, val label: String, val clusterSize: Int, val seed: Long, val timestampSec: Long,
    /** ext3 / ext4 の機能 (mke2fs 風: "^resize_inode,64bit")。空 = 既定 */
    val features: String = "",
)

/**
 * 1 つのファイルシステムの「作れるか」の問い合わせ結果 (ネイティブの Mkfs 容量・機能の問い合わせ。docs/mkfs-extended.md)。
 * [sizeOk] / [reasonCode] / [reason] は、問い合わせた範囲の大きさで作れるか。[clusterOk] は、その範囲で計画が通るクラスタ (ブロック) サイズ。
 * [features] は ext の機能 ("name" = 既定で有効、"^name" = 既定で無効)。
 */
data class FormatCap(
    val fs: String, val supported: Boolean, val experimental: Boolean, val minBytes: Long, val maxBytes: Long,
    val clusterSizes: List<Int>, val clusterOk: List<Int>, val defaultCluster: Int, val labelMax: Int, val labelCharset: String,
    val features: List<String>, val sizeOk: Boolean, val reasonCode: String, val reason: String,
)

/** パーティション探索の指定。[seed] は復元案の GUID を決める (探索のあとの適用で同じ計画になるよう、画面が最初に決めて持ち回る)。 */
data class ScanSpec(val full: Boolean, val minConfidence: Int, val seed: Long)

/** 復元案 1 つ。[json] はネイティブの案 (index / id / title / confidence / planOk / notes / partitions ...)、[plan] はその案を積んだ編集の計画 (書かない。変更後の表を含む)。 */
class ScanProposal(val json: JSONObject, val plan: PartitionPlan?, val planError: String?)

/** 探索の結果。[cancelled] = 取り消されて途中まで (この場合 [proposals] は空)。 */
class ScanReport(val result: JSONObject, val cancelled: Boolean, val proposals: List<ScanProposal>)

/** ブートセクタ修復の結果。[after] = 適用後に検査し直した結果 (inspection の JSON)。 */
data class BootDone(val result: BootApplyResult, val backupName: String?, val after: JSONObject?)

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
    /** パーティション探索・ブートセクタ検査 / 修復のジョブ (進捗は phase = table / scan / verify / backup / write) */
    fun newScanJob(): DiskOpJob

    // ---- 検査 (読み取り専用)
    fun check(t: DiskTarget, r: DiskRange, job: DiskOpJob): Outcome<FsCheckResult>

    // ---- フォーマット
    /** 作れるファイルシステムと、[r] の大きさで作れるか・使えるクラスタサイズ・ラベルの上限・ext の機能 (ネイティブへの問い合わせ。何も開かない・書かない)。名前は小文字: ext2 / exfat / fat32 ... */
    fun formatCaps(r: DiskRange, sectorSize: Int): List<FormatCap>
    fun formatPlan(t: DiskTarget, r: DiskRange, spec: FormatSpec): Outcome<MkfsPlan>
    /** 計画を作り直して [expectedPhrase] と一致することを確かめてから、[typed] で適用する (書き込みモード・マウント状態もここで確かめる) */
    fun formatApply(t: DiskTarget, r: DiskRange, spec: FormatSpec, expectedPhrase: String, typed: String, job: DiskOpJob): Outcome<FormatDone>

    // ---- パーティション編集
    fun partitionGpt(t: DiskTarget): Outcome<JSONObject>
    fun partitionPlan(t: DiskTarget, ops: List<PartOp>): Outcome<PartitionPlan>
    fun partitionApply(t: DiskTarget, ops: List<PartOp>, expectedPhrase: String, typed: String): Outcome<PartitionDone>
    /** 今のパーティション表のバックアップを [dest] (SAF) へ保存する。戻り値 = バイト数 */
    fun partitionBackupTo(t: DiskTarget, dest: Uri): Outcome<Int>

    // ---- 失われたパーティションを探す (TestDisk 相当。探索は読み取り専用。docs/partition-scan.md)
    /** 探索して、候補と復元案 (案ごとの計画つき) を返す。読み取り専用で開くだけで、何も書かない */
    fun scanRun(t: DiskTarget, spec: ScanSpec, job: DiskOpJob): Outcome<ScanReport>
    /**
     * 復元案を適用する。**探索と案の計画をやり直し**、案の id と確認文字列が画面に見せたものと一致することを確かめてから、[typed] で表だけを書く
     * (書く前に表の控えをアプリ内へ保存し、書いたあと読み戻して確かめる。書き込みモード・マウント状態もここで確かめる)。
     */
    fun scanApply(t: DiskTarget, spec: ScanSpec, proposalId: String, proposalIndex: Int, expectedPhrase: String, typed: String, job: DiskOpJob): Outcome<PartitionDone>

    // ---- ブートセクタの検査 (読み取り専用) / 修復 (書き込み)。ntfsfix のような自動修復は使わない
    fun bootInspect(t: DiskTarget, r: DiskRange, job: DiskOpJob): Outcome<JSONObject>
    /** 修復の計画 (書かない)。[repairId] / [source] は検査結果の repairs[].id / source */
    fun bootPlan(t: DiskTarget, r: DiskRange, repairId: String, source: Long): Outcome<JSONObject>
    /** 計画を作り直して [expectedPhrase] と一致することを確かめてから、[typed] で適用する (書く前に元の内容をアプリ内へ保存。書き込みモード・マウント状態もここで確かめる) */
    fun bootApply(t: DiskTarget, r: DiskRange, repairId: String, source: Long, expectedPhrase: String, typed: String, job: DiskOpJob): Outcome<BootDone>

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
