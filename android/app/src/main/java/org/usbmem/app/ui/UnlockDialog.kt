package org.usbmem.app.ui

import android.net.Uri
import androidx.activity.compose.rememberLauncherForActivityResult
import androidx.activity.result.contract.ActivityResultContracts
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardActions
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Checkbox
import androidx.compose.material3.FilterChip
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.input.ImeAction
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.DialogProperties
import org.usbmem.app.CryptoInfo
import org.usbmem.app.CryptoMount
import org.usbmem.app.R
import org.usbmem.app.SecretVault
import org.usbmem.app.UnlockCredentials
import org.usbmem.app.VaultSupport

/**
 * 解錠ダイアログに出す内容 (人に見せてよい情報だけ。資格情報は含まない)。[error] = 直前の解錠の失敗 (認証失敗など)。付いていれば、そのまま再入力できる。
 * [startBytes] / [sizeBytes] = 暗号ボリュームの範囲 (親デバイス上)。
 */
data class UnlockPrompt(
    val startBytes: Long, val sizeBytes: Long, val label: String,
    val format: String, val cipher: String,
    val canPassword: Boolean, val canRecoveryKey: Boolean, val canKeyFile: Boolean,
    /** 保護が一時停止中 (BitLocker の clear key) などで、資格情報なしで解錠できる */
    val noSecret: Boolean,
    /** false = 非対応の方式・TPM のみなど。解錠の操作を出さず、[note] を見せる */
    val usable: Boolean,
    val note: String,
    /** このビルド・構成なら書き込み可で開ける (core の write_supported) */
    val writeSupported: Boolean,
    val writeNote: String,
    val error: String? = null,
    /** パスワードの記憶 ([SecretVault]) の鍵になるボリューム ID。空 = 識別子が取れない (記憶の対象外)。秘密ではない。 */
    val volumeId: String = "",
    /** このボリュームのパスワードを記憶できる (この端末が対応・パスワードで解錠できる・識別子がある) */
    val canRemember: Boolean = false,
    /** すでに記憶している (生体認証 / 画面ロックで解錠できる) */
    val remembered: Boolean = false,
    /** 記憶が使えない理由 (パスワードで解錠できるのに記憶の機能を出せないとき)。null = 使える、または記憶の対象外 (理由を出す意味が無い) */
    val rememberUnavailable: VaultSupport? = null,
) {
    companion object {
        /** [vault] = パスワードの記憶。null なら記憶の機能を出さない。 */
        fun of(info: CryptoInfo, startBytes: Long, sizeBytes: Long, label: String, vault: SecretVault? = null): UnlockPrompt {
            val usable = info.supported && !info.tpmOnly
            val vid = SecretVault.idOf(info)
            val eligible = vault != null && usable && info.canPassword && vid.isNotEmpty()
            val state = if (eligible) vault!!.supportState() else null
            val can = state == VaultSupport.Supported
            return UnlockPrompt(
                startBytes, sizeBytes, label, CryptoMount.displayFormat(info), info.cipher,
                info.canPassword, info.canRecoveryKey, info.canKeyFile, info.unlockedWithoutSecret,
                usable = usable, note = info.note, writeSupported = info.writeSupported, writeNote = info.writeNote,
                volumeId = vid, canRemember = can, remembered = can && vault!!.has(vid),
                rememberUnavailable = state?.takeIf { it != VaultSupport.Supported },
            )
        }
    }
}

/**
 * ダイアログが集めた入力。[cred] のバイト列は、受け取った側が使い終えたらゼロ埋めする ([CryptoMount.unlock] が最後に埋める)。
 * [keyFileUri] = BEK ファイル (中身は解錠の直前に読む。UI の状態には載せない)。[write] = 書き込み可で開く要求。
 * パスワードの記憶: [vaultId] = 記憶の鍵 ([UnlockPrompt.volumeId])。[remember] = 解錠に成功したあと、このパスワードを記憶する (パスワード方式のときだけ)。
 * [useRemembered] = 入力ではなく、記憶したパスワードを (生体認証 / 画面ロックのあとで) 使って解錠する ([cred] は空)。
 */
class UnlockInput(
    val cred: UnlockCredentials, val method: CryptoMount.Method, val keyFileUri: Uri?, val write: Boolean,
    val vaultId: String = "", val remember: Boolean = false, val useRemembered: Boolean = false,
)

/** 暗号ボリュームを調べた結果。[prompt] があれば解錠ダイアログを出す。なければ [notice] (暗号ではない・開けない) を見せる。 */
data class CryptoProbeResult(val prompt: UnlockPrompt? = null, val notice: Notice? = null)

/** 解錠の結果。[retryError] があれば、ダイアログを開いたまま、その文言を出して再入力させる (認証失敗など)。なければ [notice] を見せてダイアログを閉じる。 */
data class CryptoUnlockResult(
    val notice: Notice? = null, val retryError: String? = null, val noload: CryptoNoloadPrompt? = null,
    /** 非 null = 解錠のあとの「記憶している」状態 (記憶を捨てた・保存したとき)。null = 変化なし */
    val rememberedNow: Boolean? = null,
)

/**
 * 解錠はできたが、中の ext3/ext4 などのジャーナルが未再生 (EUCLEAN) で、「読み取り専用で開く」確認を待っているボリューム (資格情報は含まない)。
 * [id] = 確認・キャンセルに渡す識別子 (VolumeRegistry の保留)。[writeRequested] = 書き込み可での解錠を選んでいた (読み取り専用でしか開けないことを文言で示す)。[dirty] / [journal] = 保留の中身 (確認ダイアログの種類を決める)。
 */
data class CryptoNoloadPrompt(
    val id: String, val label: String, val writeRequested: Boolean,
    /** 保留の中に、ダーティフラグだけの NTFS (読み取り専用か書き込み可かを選ぶ) がある */
    val dirty: Boolean = false,
    /** 保留の中に、ジャーナル未再生の FS (読み取り専用でしか開けない) がある */
    val journal: Boolean = true,
) {
    companion object {
        fun of(id: String, label: String, writeRequested: Boolean, r: org.usbmem.app.PendingRecovery) = CryptoNoloadPrompt(id, label, writeRequested, dirty = r.hasDirty, journal = r.hasJournal)
    }
}

/** 解錠済みの暗号ボリューム (中の FS) 1 つ。[id] = VolumeRegistry の id (ロックの操作に渡す)。 */
data class CryptoVolumeRow(val id: String, val label: String, val fsName: String, val format: String, val writable: Boolean)

/** 解錠ダイアログを出す対象 (パーティションの範囲と、ダイアログの内容)。 */
data class UnlockTarget(val startBytes: Long, val sizeBytes: Long, val prompt: UnlockPrompt)

/** 回復キーの入力を、数字 48 桁なら 6 桁ずつのハイフン区切りにする (そうでなければ入力のまま)。 */
internal fun normalizeRecoveryKey(s: String): String {
    val digits = s.filter { it in '0'..'9' }
    return if (digits.length == 48 && s.all { it in '0'..'9' || it == '-' || it == ' ' }) digits.chunked(6).joinToString("-") else s.trim()
}

/**
 * 暗号ボリュームの解錠ダイアログ。パスワード (マスク表示・表示切替)、BitLocker の回復キー (48 桁)・BEK ファイル (SAF) に対応する。
 * 入力は `remember` (Activity の状態保存・SharedPreferences には載せない) で持ち、送信した瞬間に空にする。
 * 文字列 (String) は JVM のヒープに残り消せないので、できるだけ短い間しか持たない。ByteArray に変えて渡した分は受け取った側がゼロ埋めする。
 *
 * @param writeAvailable 書き込み可で開ける状況か (USB を書き込み可で開いている / 画面の書き込み許可がオン)。false なら書き込みの切り替えは無効。
 * @param writeInitial 書き込みの切り替えの初期値 (画面の「書き込みを許可」がオンなら true)
 * @param busy 解錠中 (入力を無効にして、進行表示を出す)
 */
@Composable
fun UnlockDialog(
    prompt: UnlockPrompt, writeAvailable: Boolean, writeInitial: Boolean, busy: Boolean,
    onSubmit: (UnlockInput) -> Unit, onCancel: () -> Unit,
    /** 記憶したパスワードを忘れる (記憶を削除する)。呼び出し側が [UnlockPrompt.remembered] を false にして出し直す */
    onForget: () -> Unit = {},
) {
    val methods = buildList {
        if (prompt.canPassword) add(CryptoMount.Method.Password)
        if (prompt.canRecoveryKey) add(CryptoMount.Method.RecoveryKey)
        if (prompt.canKeyFile) add(CryptoMount.Method.KeyFile)
    }
    var method by remember { mutableStateOf(methods.firstOrNull() ?: CryptoMount.Method.Password) }
    var password by remember { mutableStateOf("") }
    var showPassword by remember { mutableStateOf(false) }
    var recovery by remember { mutableStateOf("") }
    var keyFileUri by remember { mutableStateOf<Uri?>(null) }
    val writePossible = writeAvailable && prompt.writeSupported
    var write by remember { mutableStateOf(writePossible && writeInitial) }
    var confirmWrite by remember { mutableStateOf(false) }
    var rememberIt by remember { mutableStateOf(false) }
    var confirmForget by remember { mutableStateOf(false) }
    val picker = rememberLauncherForActivityResult(ActivityResultContracts.OpenDocument()) { uri: Uri? -> if (uri != null) keyFileUri = uri }

    val recoveryOk = recovery.count { it in '0'..'9' } == 48
    val canSubmit = !busy && prompt.usable && (prompt.noSecret || when (method) {
        CryptoMount.Method.Password -> password.isNotEmpty()
        CryptoMount.Method.RecoveryKey -> recoveryOk
        CryptoMount.Method.KeyFile -> keyFileUri != null
    })

    fun submit() {
        val c = UnlockCredentials()
        when (method) {
            CryptoMount.Method.Password -> if (password.isNotEmpty()) c.password = password.toByteArray(Charsets.UTF_8)
            CryptoMount.Method.RecoveryKey -> if (recovery.isNotEmpty()) c.recoveryKey = normalizeRecoveryKey(recovery).toByteArray(Charsets.UTF_8)
            CryptoMount.Method.KeyFile -> {}
        }
        val input = UnlockInput(
            c, method, if (method == CryptoMount.Method.KeyFile) keyFileUri else null, write && writePossible,
            vaultId = prompt.volumeId, remember = rememberIt && prompt.canRemember && !prompt.remembered && method == CryptoMount.Method.Password && password.isNotEmpty(),
        )
        // 送信したら、入力欄の文字列への参照を捨てる (失敗して再入力になっても、前回の内容は残さない)
        password = ""; recovery = ""; showPassword = false
        onSubmit(input)
    }

    /** 記憶したパスワードで解錠する (入力は使わない。生体認証 / 画面ロックは受け取った側が求める)。 */
    fun submitRemembered() {
        onSubmit(UnlockInput(UnlockCredentials(), CryptoMount.Method.Password, null, write && writePossible, vaultId = prompt.volumeId, useRemembered = true))
    }

    AlertDialog(
        onDismissRequest = { if (!busy) onCancel() },
        // キーボードが出ても、「解錠」「キャンセル」が隠れない (ダイアログがキーボードの高さぶん縮み、本文は下のスクロールで読める)
        modifier = Modifier.testTag("unlock_dialog").imePadding(),
        properties = DialogProperties(decorFitsSystemWindows = false),
        title = { Text(stringResource(R.string.unlock_title)) },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(12.dp)) {
                Text(prompt.label, style = MaterialTheme.typography.titleSmall)
                Text(stringResource(R.string.unlock_subtitle, prompt.format, prompt.cipher), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                if (!prompt.usable) Banner(Notice(Severity.Warning, stringResource(R.string.unlock_unusable, prompt.note)))
                prompt.error?.let { Banner(Notice(Severity.Error, it), Modifier.testTag("unlock_error")) }
                if (prompt.usable && prompt.noSecret) Text(stringResource(R.string.unlock_no_secret), style = MaterialTheme.typography.bodyMedium)
                if (prompt.usable && prompt.canRemember && prompt.remembered) {
                    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        Text(stringResource(R.string.unlock_remembered_hint), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                            OutlinedButton(onClick = { submitRemembered() }, enabled = !busy, modifier = Modifier.weight(1f).testTag("unlock_remembered")) {
                                Text(stringResource(R.string.unlock_remembered_button))
                            }
                            TextButton(onClick = { confirmForget = true }, enabled = !busy, modifier = Modifier.testTag("unlock_forget")) { Text(stringResource(R.string.unlock_forget)) }
                        }
                    }
                }
                if (prompt.usable && methods.size > 1) {
                    Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                        methods.forEach { m ->
                            FilterChip(
                                selected = method == m, onClick = { method = m }, enabled = !busy,
                                label = { Text(stringResource(when (m) { CryptoMount.Method.Password -> R.string.unlock_method_password; CryptoMount.Method.RecoveryKey -> R.string.unlock_method_recovery; CryptoMount.Method.KeyFile -> R.string.unlock_method_keyfile })) },
                            )
                        }
                    }
                }
                if (prompt.usable) when (method) {
                    CryptoMount.Method.Password -> Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        OutlinedTextField(
                            value = password, onValueChange = { password = it }, enabled = !busy, singleLine = true,
                            label = { Text(stringResource(R.string.unlock_field_password)) },
                            visualTransformation = if (showPassword) VisualTransformation.None else PasswordVisualTransformation(),
                            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password, imeAction = ImeAction.Done),
                            keyboardActions = KeyboardActions(onDone = { if (canSubmit) submit() }),
                            modifier = Modifier.fillMaxWidth().testTag("unlock_password"),
                        )
                        TextButton(onClick = { showPassword = !showPassword }, enabled = !busy, modifier = Modifier.testTag("unlock_show_toggle")) {
                            Text(stringResource(if (showPassword) R.string.unlock_hide else R.string.unlock_show))
                        }
                        // 記憶できない端末では、理由を一言 (黙って消えない)
                        prompt.rememberUnavailable?.let { why ->
                            Text(
                                stringResource(when (why) {
                                    VaultSupport.OsTooOld -> R.string.unlock_remember_unavailable_os
                                    VaultSupport.NoCredential -> R.string.unlock_remember_unavailable_no_lock
                                    else -> R.string.unlock_remember_unavailable_other
                                }),
                                style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.testTag("unlock_remember_unavailable"),
                            )
                        }
                        // パスワードの記憶 (既定はオフ。解錠に成功したあとにだけ保存する)
                        if (prompt.canRemember && !prompt.remembered) {
                            Row(verticalAlignment = Alignment.CenterVertically) {
                                Checkbox(checked = rememberIt, onCheckedChange = { rememberIt = it }, enabled = !busy, modifier = Modifier.testTag("unlock_remember"))
                                Column(Modifier.weight(1f)) {
                                    Text(stringResource(R.string.unlock_remember_label), style = MaterialTheme.typography.bodyMedium)
                                    Text(stringResource(R.string.unlock_remember_desc), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                                }
                            }
                        }
                    }
                    CryptoMount.Method.RecoveryKey -> Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        OutlinedTextField(
                            value = recovery, onValueChange = { recovery = it }, enabled = !busy, singleLine = true,
                            label = { Text(stringResource(R.string.unlock_field_recovery)) },
                            visualTransformation = PasswordVisualTransformation(),
                            keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password, imeAction = ImeAction.Done),
                            keyboardActions = KeyboardActions(onDone = { if (canSubmit) submit() }),
                            modifier = Modifier.fillMaxWidth().testTag("unlock_recovery"),
                        )
                        Text(stringResource(R.string.unlock_recovery_hint), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                    CryptoMount.Method.KeyFile -> Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                        OutlinedButton(onClick = { picker.launch(arrayOf("*/*")) }, enabled = !busy, modifier = Modifier.fillMaxWidth().testTag("unlock_pick_bek")) { Text(stringResource(R.string.unlock_pick_bek)) }
                        Text(
                            keyFileUri?.let { stringResource(R.string.unlock_bek_selected, it.lastPathSegment?.substringAfterLast('/') ?: "") } ?: stringResource(R.string.unlock_bek_none),
                            style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                }
                // ---- 書き込み (既定は読み取り専用。他の書き込みと同じく、明示的に有効にする。暗号ボリュームの書き込みは実験的)
                if (prompt.usable) Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(stringResource(R.string.unlock_write_title), style = MaterialTheme.typography.bodyLarge)
                        Text(
                            when {
                                !writeAvailable -> stringResource(R.string.unlock_write_off_device)
                                !prompt.writeSupported -> stringResource(R.string.unlock_write_off_build, prompt.writeNote)
                                else -> stringResource(R.string.unlock_write_desc)
                            },
                            style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                    Switch(
                        checked = write && writePossible, enabled = writePossible && !busy, modifier = Modifier.testTag("unlock_write_switch"),
                        onCheckedChange = { if (it) confirmWrite = true else write = false },
                    )
                }
                // 「記憶する」のチェックボックスはパスワードの入力欄の下 (この文より上) にあり、回復キー / BEK の入力のときは出ない: そのときは「記憶する」に触れない文にする
                Text(stringResource(if (prompt.canRemember && !prompt.remembered && method == CryptoMount.Method.Password) R.string.unlock_not_saved else R.string.unlock_not_saved_plain), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                if (busy) {
                    LinearProgressIndicator(Modifier.fillMaxWidth())
                    Text(stringResource(R.string.unlock_progress), style = MaterialTheme.typography.bodySmall)
                }
            }
        },
        confirmButton = {
            TextButton(onClick = { submit() }, enabled = canSubmit, modifier = Modifier.testTag("unlock_submit")) {
                Text(stringResource(if (write && writePossible) R.string.unlock_button_write else R.string.unlock_button))
            }
        },
        dismissButton = { TextButton(onClick = onCancel, enabled = !busy, modifier = Modifier.testTag("unlock_cancel")) { Text(stringResource(R.string.common_cancel)) } },
    )
    if (confirmForget) {
        ConfirmDialog(
            title = stringResource(R.string.unlock_forget_confirm_title),
            text = stringResource(R.string.unlock_forget_confirm_text),
            confirmLabel = stringResource(R.string.unlock_forget_confirm),
            onConfirm = { confirmForget = false; onForget() },
            onDismiss = { confirmForget = false },
            subject = prompt.label,
        )
    }
    if (confirmWrite) {
        ConfirmDialog(
            title = stringResource(R.string.unlock_write_confirm_title),
            text = stringResource(R.string.unlock_write_confirm_text),
            confirmLabel = stringResource(R.string.unlock_write_confirm),
            onConfirm = { confirmWrite = false; write = true },
            onDismiss = { confirmWrite = false },
        )
    }
}
