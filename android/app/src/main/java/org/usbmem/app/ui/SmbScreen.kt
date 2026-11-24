// SPDX-License-Identifier: Apache-2.0
package org.usbmem.app.ui

import android.content.Context
import android.content.ContextWrapper
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.imePadding
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material3.AlertDialog
import androidx.compose.material3.Button
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
import androidx.compose.runtime.DisposableEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberUpdatedState
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.testTag
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.contentDescription
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.text.input.PasswordVisualTransformation
import androidx.compose.ui.text.input.VisualTransformation
import androidx.compose.ui.unit.dp
import androidx.compose.ui.window.DialogProperties
import androidx.lifecycle.Lifecycle
import androidx.lifecycle.LifecycleEventObserver
import androidx.lifecycle.LifecycleOwner
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.usbmem.app.R
import org.usbmem.app.RememberResult
import org.usbmem.app.VaultSupport
import org.usbmem.app.smb.SmbCancel
import org.usbmem.app.smb.SmbConnection
import org.usbmem.app.smb.SmbController
import org.usbmem.app.smb.SmbErrorKind
import org.usbmem.app.smb.SmbErrors
import org.usbmem.app.smb.SmbException
import org.usbmem.app.smb.SmbInputError
import org.usbmem.app.smb.SmbMinVersion
import org.usbmem.app.smb.SmbMounts
import org.usbmem.app.smb.SmbRecalled
import org.usbmem.app.smb.SmbSecrets
import org.usbmem.app.smb.SmbStore
import org.usbmem.app.smb.SmbTestResult

/** 失敗の種類 → 画面の文言 (サーバー・SMBJ の文字列は出さない) */
object SmbUiText {
    /**
     * 失敗 → 文言。接続できなかった (Network) は理由 (ホスト不明 / 拒否 / 経路なし / 時間切れ) で、署名・暗号化・バージョンの不一致 (Protocol) は [conn] の設定で、案内を分ける。
     * 通信が切れた (Disconnected / Timeout) には、もう一度「マウント」する案内が付く (文言側)。
     */
    fun error(ctx: Context, e: Throwable, conn: SmbConnection? = null): String {
        val m = SmbErrors.map(e)
        val res = when (m.kind) {
            SmbErrorKind.Network -> when (SmbErrors.netReason(m)) {
                SmbErrors.NetReason.HostUnknown -> R.string.smb_error_host_unknown
                SmbErrors.NetReason.Refused -> R.string.smb_error_refused
                SmbErrors.NetReason.Unreachable -> R.string.smb_error_unreachable
                SmbErrors.NetReason.ConnectTimeout -> R.string.smb_error_connect_timeout
                SmbErrors.NetReason.Other -> R.string.smb_error_network
            }
            SmbErrorKind.Protocol -> when {
                conn == null -> R.string.smb_error_protocol
                conn.requireEncryption -> R.string.smb_error_protocol_encryption
                conn.requireSigning -> R.string.smb_error_protocol_signing
                conn.minVersion != SmbMinVersion.V202 && conn.minVersion != SmbMinVersion.V21 -> R.string.smb_error_protocol_version
                else -> R.string.smb_error_protocol
            }
            else -> null
        }
        if (res == null) return error(ctx, m.kind)
        return if (res == R.string.smb_error_protocol_version && conn != null) ctx.getString(res, conn.minVersion.key) else ctx.getString(res)
    }

    fun error(ctx: Context, k: SmbErrorKind): String = ctx.getString(when (k) {
        SmbErrorKind.Auth -> R.string.smb_error_auth
        SmbErrorKind.AccessDenied -> R.string.smb_error_access
        SmbErrorKind.NotFound -> R.string.smb_error_not_found
        SmbErrorKind.ShareNotFound -> R.string.smb_error_share_not_found
        SmbErrorKind.Exists -> R.string.smb_error_exists
        SmbErrorKind.NotEmpty -> R.string.smb_error_not_empty
        SmbErrorKind.Network -> R.string.smb_error_network
        SmbErrorKind.Disconnected -> R.string.smb_error_disconnected
        SmbErrorKind.Timeout -> R.string.smb_error_timeout
        SmbErrorKind.NoSpace -> R.string.smb_error_no_space
        SmbErrorKind.Busy -> R.string.smb_error_busy
        SmbErrorKind.Protocol -> R.string.smb_error_protocol
        SmbErrorKind.InvalidName -> R.string.smb_error_invalid_name
        SmbErrorKind.Cancelled -> R.string.smb_error_cancelled
        SmbErrorKind.ReadOnly -> R.string.smb_error_read_only
        SmbErrorKind.Other -> R.string.smb_error_other
    })

    fun input(ctx: Context, e: SmbInputError): String = ctx.getString(when (e) {
        SmbInputError.Name -> R.string.smb_err_name
        SmbInputError.Host -> R.string.smb_err_host
        SmbInputError.Port -> R.string.smb_err_port
        SmbInputError.Share -> R.string.smb_err_share
        SmbInputError.Domain -> R.string.smb_err_domain
        SmbInputError.User -> R.string.smb_err_user
    })
}

private val SMB_DIALOG = DialogProperties(usePlatformDefaultWidth = false, decorFitsSystemWindows = false)
private val SMB_DIALOG_MARGIN = 24.dp

/** 画面が見えなくなったとき (ON_STOP) に [onStop] を呼ぶ。入力中のパスワードを、画面の外へ持ち越さないために使う。 */
@Composable
private fun OnStopEffect(onStop: () -> Unit) {
    val ctx = LocalContext.current
    val current by rememberUpdatedState(onStop)
    DisposableEffect(ctx) {
        var c: Context? = ctx
        var owner: LifecycleOwner? = null
        while (c != null) {
            if (c is LifecycleOwner) { owner = c; break }
            c = (c as? ContextWrapper)?.baseContext
        }
        val obs = LifecycleEventObserver { _, e -> if (e == Lifecycle.Event.ON_STOP) current() }
        owner?.lifecycle?.addObserver(obs)
        onDispose { owner?.lifecycle?.removeObserver(obs) }
    }
}

private data class SmbRow(val conn: SmbConnection, val mounted: Boolean, val writable: Boolean, val hasPassword: Boolean)

/** 編集の対象 (conn = null は新規) */
private class SmbEditTarget(val conn: SmbConnection?)

/**
 * ネットワーク (SMB 共有) の画面。接続の一覧・追加・編集・削除、接続テスト、マウント / 解除。
 * 実処理は [SmbController] (ネットワーク・Keystore なので IO スレッド)。**パスワード**は、入力欄 (Compose の状態) から送信の直前に配列へ移して欄を空にし、使ったあと配列をゼロで埋める。
 * 画面が止まる (ON_STOP)・ダイアログを閉じるときも、入力欄を空にする。
 */
@Composable
fun SmbScreen(modifier: Modifier = Modifier) {
    val ctx = LocalContext.current.applicationContext
    val scope = rememberUiScope()
    var rows by remember { mutableStateOf<List<SmbRow>>(emptyList()) }
    var notice by remember { mutableStateOf<Notice?>(null) }
    var edit by remember { mutableStateOf<SmbEditTarget?>(null) }
    var mountFor by remember { mutableStateOf<SmbConnection?>(null) }
    var deleteFor by remember { mutableStateOf<SmbConnection?>(null) }
    var busy by remember { mutableStateOf(false) }

    suspend fun load() {
        val (list, lost) = withContext(Dispatchers.IO) {
            SmbMounts.reapDead(ctx)
            val conns = SmbStore(ctx).all()
            val l = conns.map { c ->
                val m = SmbMounts.byConnection(c.id)
                SmbRow(c, m != null, m?.writable ?: false, SmbController.hasSavedPassword(ctx, c))
            }
            l to conns.mapNotNull { c -> SmbMounts.takeLost(c.id)?.let { c.name } }
        }
        rows = list
        if (lost.isNotEmpty()) notice = Notice(Severity.Warning, ctx.getString(R.string.smb_notice_lost, lost.joinToString(", ")), ctx.getString(R.string.smb_notice_lost_detail))
    }
    fun reload() { scope.launch { load() } }

    // 切断 (ネットワークが切れた) を拾うため、見ている間は定期的に取り直す
    LaunchedEffectUi(Unit) { while (true) { load(); delay(4000) } }

    Column(modifier, verticalArrangement = Arrangement.spacedBy(16.dp)) {
        SectionCard(stringResource(R.string.smb_title), subtitle = stringResource(R.string.smb_subtitle)) {
            Button(onClick = { edit = SmbEditTarget(null) }, enabled = !busy, modifier = Modifier.fillMaxWidth().testTag("smb_add")) { Text(stringResource(R.string.smb_add)) }
            Text(stringResource(R.string.smb_privacy), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
        if (rows.isEmpty()) {
            Text(stringResource(R.string.smb_none), style = MaterialTheme.typography.bodyMedium, color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.testTag("smb_none"))
        }
        rows.forEach { r ->
            val c = r.conn
            val cdMount = stringResource(R.string.smb_cd_mount, c.name)
            val cdUnmount = stringResource(R.string.smb_cd_unmount, c.name)
            val cdEdit = stringResource(R.string.smb_cd_edit, c.name)
            val cdDelete = stringResource(R.string.smb_cd_delete, c.name)
            SectionCard(
                c.name, subtitle = c.target(),
                trailing = { Chip(stringResource(if (r.mounted) R.string.smb_chip_mounted else R.string.smb_chip_not_mounted), severity = if (r.mounted) Severity.Success else Severity.Info) },
                modifier = Modifier.testTag("smb_row_" + c.id),
            ) {
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                    if (r.mounted) Chip(stringResource(if (r.writable) R.string.smb_chip_write else R.string.smb_chip_read_only), severity = if (r.writable) Severity.Warning else Severity.Success)
                    if (r.hasPassword) Chip(stringResource(R.string.smb_chip_saved_password))
                    if (c.requireEncryption) Chip(stringResource(R.string.smb_chip_encrypted))
                }
                if (r.mounted) Text(stringResource(R.string.smb_state_mounted, c.name), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                    if (r.mounted) {
                        OutlinedButton(
                            onClick = {
                                scope.launch {
                                    busy = true
                                    withContext(Dispatchers.IO) { SmbController.unmount(ctx, c) }
                                    busy = false
                                    notice = Notice(Severity.Info, ctx.getString(R.string.smb_notice_unmounted, c.name))
                                    load()
                                }
                            },
                            enabled = !busy, modifier = Modifier.weight(1f).semantics { contentDescription = cdUnmount }.testTag("smb_unmount_" + c.id),
                        ) { Text(stringResource(R.string.smb_button_unmount)) }
                    } else {
                        Button(
                            onClick = { mountFor = c }, enabled = !busy,
                            modifier = Modifier.weight(1f).semantics { contentDescription = cdMount }.testTag("smb_mount_" + c.id),
                        ) { Text(stringResource(R.string.smb_button_mount)) }
                    }
                    TextButton(onClick = { edit = SmbEditTarget(c) }, enabled = !busy, modifier = Modifier.semantics { contentDescription = cdEdit }.testTag("smb_edit_" + c.id)) { Text(stringResource(R.string.smb_button_edit)) }
                    TextButton(onClick = { deleteFor = c }, enabled = !busy, modifier = Modifier.semantics { contentDescription = cdDelete }.testTag("smb_delete_" + c.id)) { Text(stringResource(R.string.smb_button_delete)) }
                }
            }
        }
        notice?.let { Banner(it, Modifier.testTag("smb_notice"), scrollIntoView = true) }
    }

    edit?.let { t ->
        val saved = t.conn?.let { c -> rows.firstOrNull { it.conn.id == c.id }?.hasPassword } ?: false
        SmbEditDialog(
            initial = t.conn, hasSavedPassword = saved,
            onDismiss = { edit = null },
            onSaved = { c, n -> edit = null; notice = n; reload() },
        )
    }
    mountFor?.let { c ->
        val saved = rows.firstOrNull { it.conn.id == c.id }?.hasPassword ?: false
        SmbMountDialog(
            conn = c, hasSavedPassword = saved,
            onDismiss = { mountFor = null },
            onDone = { n -> mountFor = null; notice = n; reload() },
        )
    }
    deleteFor?.let { c ->
        ConfirmDialog(
            title = stringResource(R.string.smb_delete_title),
            text = stringResource(R.string.smb_delete_text),
            confirmLabel = stringResource(R.string.smb_delete_confirm),
            subject = c.name,
            onConfirm = {
                deleteFor = null
                scope.launch {
                    busy = true
                    withContext(Dispatchers.IO) { SmbController.delete(ctx, c) }
                    busy = false
                    notice = Notice(Severity.Info, ctx.getString(R.string.smb_notice_deleted, c.name))
                    load()
                }
            },
            onDismiss = { deleteFor = null },
        )
    }
}

/** 接続の追加・編集。キャンセル (外側のタップ・戻る) では何も保存しない。 */
@Composable
private fun SmbEditDialog(initial: SmbConnection?, hasSavedPassword: Boolean, onDismiss: () -> Unit, onSaved: (SmbConnection, Notice) -> Unit) {
    val ctx = LocalContext.current.applicationContext
    val scope = rememberUiScope()
    var name by remember { mutableStateOf(initial?.name ?: "") }
    var host by remember { mutableStateOf(initial?.host ?: "") }
    var port by remember { mutableStateOf((initial?.port ?: SmbConnection.DEFAULT_PORT).toString()) }
    var share by remember { mutableStateOf(initial?.share ?: "") }
    var domain by remember { mutableStateOf(initial?.domain ?: "") }
    var user by remember { mutableStateOf(initial?.user ?: "") }
    var password by remember { mutableStateOf("") }
    var showPassword by remember { mutableStateOf(false) }
    var minVersion by remember { mutableStateOf(initial?.minVersion ?: SmbMinVersion.V21) }
    var encrypt by remember { mutableStateOf(initial?.requireEncryption ?: false) }
    var signing by remember { mutableStateOf(initial?.requireSigning ?: false) }
    var savePassword by remember { mutableStateOf(hasSavedPassword) }
    var attempted by remember { mutableStateOf(false) }   // 保存・接続テストを一度押したら、以後は入力のたびに欄の誤りを更新する
    var testNotice by remember { mutableStateOf<Notice?>(null) }
    var testing by remember { mutableStateOf(false) }
    var saving by remember { mutableStateOf(false) }
    var attempt by remember { mutableStateOf<SmbCancel?>(null) }
    val vaultState = remember { SmbController.support(ctx) }
    val canSavePassword = vaultState == VaultSupport.Supported

    // 画面が止まる・ダイアログが消えるときは、入力中のパスワードを消し、通信中なら取り消す
    OnStopEffect { password = ""; showPassword = false }
    DisposableEffect(Unit) { onDispose { password = ""; attempt?.cancel() } }

    /** 入力の正規化: ホストの前後の空白と IPv6 の角かっこ (「[fe80::1]」と貼り付けられたとき) を取る */
    fun hostText(): String = host.trim().removePrefix("[").removeSuffix("]")
    fun portNumber(): Int = port.trim().toIntOrNull() ?: -1
    fun inputError(): SmbInputError? = SmbConnection.validateFields(name.trim(), hostText(), portNumber(), share.trim(), domain.trim(), user.trim())
    val fieldError: SmbInputError? = if (attempted) inputError() else null

    fun build(): SmbConnection? {
        attempted = true
        val p = portNumber()
        val err = inputError()
        if (err != null) return null   // 誤りの表示は fieldError (入力のたびに更新される)
        return SmbConnection(
            id = initial?.id ?: SmbConnection.newId(), name = name.trim(), host = hostText(), port = p, share = share.trim(),
            domain = domain.trim(), user = user.trim(), minVersion = minVersion, requireEncryption = encrypt, requireSigning = signing,
        )
    }

    fun test() {
        val c = build() ?: return
        if (!c.isGuest && password.isEmpty()) { testNotice = Notice(Severity.Warning, ctx.getString(R.string.smb_test_need_password)); return }
        val pw = password.toCharArray()
        password = ""   // 配列へ移したので、入力欄は空にする (テストの失敗で再入力になっても、前回の内容は残さない)
        val a = SmbCancel(); attempt = a
        testing = true; testNotice = null
        scope.launch {
            val r: Result<SmbTestResult> = withContext(Dispatchers.IO) { try { runCatching { SmbController.test(c, pw, a) } } finally { SmbSecrets.wipe(pw) } }
            testing = false; attempt = null
            testNotice = r.fold(
                onSuccess = {
                    val extra = (if (it.encrypted) ctx.getString(R.string.smb_test_ok_encrypted) else "") + (if (it.signed) ctx.getString(R.string.smb_test_ok_signed) else "")
                    Notice(Severity.Success, ctx.getString(R.string.smb_test_ok, c.target()), ctx.getString(R.string.smb_test_ok_detail, it.dialect, it.rootEntries, extra))
                },
                onFailure = { Notice(Severity.Error, SmbUiText.error(ctx, it, c)) },
            )
        }
    }

    fun save() {
        val c = build() ?: return
        val pw = password.toCharArray()
        password = ""
        val wantSave = savePassword; val hadSaved = hasSavedPassword
        saving = true
        scope.launch {
            val n: Notice = withContext(Dispatchers.IO) {
                try {
                    SmbStore(ctx).save(c)
                    val identityChanged = initial != null && (initial.host != c.host || initial.port != c.port || initial.share != c.share || initial.user != c.user || initial.domain != c.domain)
                    var detail: String? = null
                    if (!wantSave) {
                        if (hadSaved) SmbController.forgetPassword(ctx, c)
                    } else if (pw.isNotEmpty()) {
                        detail = when (SmbController.rememberPassword(ctx, c, pw)) {
                            RememberResult.Saved -> ctx.getString(R.string.smb_notice_saved_password)
                            else -> ctx.getString(R.string.smb_notice_password_not_saved)
                        }
                    } else if (identityChanged && hadSaved) {
                        SmbController.forgetPassword(ctx, c)   // 接続先・ユーザーが変わったのに、古い資格情報を持ち越さない
                    }
                    Notice(Severity.Success, ctx.getString(R.string.smb_notice_saved, c.name), detail)
                } finally { SmbSecrets.wipe(pw) }
            }
            saving = false
            onSaved(c, n)
        }
    }

    val busy = testing || saving
    AlertDialog(
        onDismissRequest = { if (!saving) onDismiss() },
        modifier = Modifier.padding(horizontal = SMB_DIALOG_MARGIN).imePadding().testTag("smb_edit_dialog"),
        properties = SMB_DIALOG,
        title = { Text(stringResource(if (initial == null) R.string.smb_edit_title_new else R.string.smb_edit_title_edit)) },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                // 先頭の誤りの表示は、まだ直っていない間だけ (直した欄の表示は、入力のたびに消える)
                fieldError?.let { Banner(Notice(Severity.Error, SmbUiText.input(ctx, it)), Modifier.testTag("smb_edit_error")) }
                OutlinedTextField(
                    value = name, onValueChange = { name = it }, enabled = !busy, singleLine = true, label = { Text(stringResource(R.string.smb_field_name)) },
                    isError = fieldError == SmbInputError.Name, supportingText = { if (fieldError == SmbInputError.Name) Text(stringResource(R.string.smb_err_name)) },
                    modifier = Modifier.fillMaxWidth().testTag("smb_field_name"),
                )
                OutlinedTextField(
                    value = host, onValueChange = { host = it }, enabled = !busy, singleLine = true, label = { Text(stringResource(R.string.smb_field_host)) },
                    isError = fieldError == SmbInputError.Host, supportingText = { if (fieldError == SmbInputError.Host) Text(stringResource(R.string.smb_err_host)) },
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri), modifier = Modifier.fillMaxWidth().testTag("smb_field_host"),
                )
                OutlinedTextField(
                    value = port, onValueChange = { port = it.filter { ch -> ch.isDigit() }.take(5) }, enabled = !busy, singleLine = true, label = { Text(stringResource(R.string.smb_field_port)) },
                    isError = fieldError == SmbInputError.Port, supportingText = { if (fieldError == SmbInputError.Port) Text(stringResource(R.string.smb_err_port)) },
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Number), modifier = Modifier.fillMaxWidth().testTag("smb_field_port"),
                )
                OutlinedTextField(
                    value = share, onValueChange = { share = it }, enabled = !busy, singleLine = true, label = { Text(stringResource(R.string.smb_field_share)) },
                    isError = fieldError == SmbInputError.Share, supportingText = { if (fieldError == SmbInputError.Share) Text(stringResource(R.string.smb_err_share)) },
                    modifier = Modifier.fillMaxWidth().testTag("smb_field_share"),
                )
                OutlinedTextField(
                    value = domain, onValueChange = { domain = it }, enabled = !busy, singleLine = true, label = { Text(stringResource(R.string.smb_field_domain)) },
                    isError = fieldError == SmbInputError.Domain, supportingText = { if (fieldError == SmbInputError.Domain) Text(stringResource(R.string.smb_err_domain)) },
                    modifier = Modifier.fillMaxWidth().testTag("smb_field_domain"),
                )
                OutlinedTextField(
                    value = user, onValueChange = { user = it }, enabled = !busy, singleLine = true, label = { Text(stringResource(R.string.smb_field_user)) },
                    isError = fieldError == SmbInputError.User, supportingText = { if (fieldError == SmbInputError.User) Text(stringResource(R.string.smb_err_user)) },
                    modifier = Modifier.fillMaxWidth().testTag("smb_field_user"),
                )
                OutlinedTextField(
                    value = password, onValueChange = { password = it }, enabled = !busy, singleLine = true, label = { Text(stringResource(R.string.smb_field_password)) },
                    visualTransformation = if (showPassword) VisualTransformation.None else PasswordVisualTransformation(),
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password), modifier = Modifier.fillMaxWidth().testTag("smb_field_password"),
                )
                TextButton(onClick = { showPassword = !showPassword }, enabled = !busy, modifier = Modifier.testTag("smb_show_password")) {
                    Text(stringResource(if (showPassword) R.string.smb_password_hide else R.string.smb_password_show))
                }
                Text(stringResource(R.string.smb_version_title), style = MaterialTheme.typography.titleSmall)
                Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
                    SmbMinVersion.entries.forEach { v ->
                        FilterChip(
                            selected = minVersion == v, onClick = { minVersion = v }, enabled = !busy,
                            label = { Text(stringResource(when (v) { SmbMinVersion.V202 -> R.string.smb_version_202; SmbMinVersion.V21 -> R.string.smb_version_21; SmbMinVersion.V30 -> R.string.smb_version_30; SmbMinVersion.V311 -> R.string.smb_version_311 })) },
                            modifier = Modifier.testTag("smb_version_" + v.key),
                        )
                    }
                }
                Text(stringResource(R.string.smb_version_hint), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(stringResource(R.string.smb_require_encryption), style = MaterialTheme.typography.bodyLarge)
                        Text(stringResource(R.string.smb_require_encryption_desc), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                    Switch(checked = encrypt, onCheckedChange = { encrypt = it }, enabled = !busy, modifier = Modifier.testTag("smb_encrypt"))
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(stringResource(R.string.smb_require_signing), style = MaterialTheme.typography.bodyLarge)
                        Text(stringResource(R.string.smb_require_signing_desc), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                    Switch(checked = signing, onCheckedChange = { signing = it }, enabled = !busy, modifier = Modifier.testTag("smb_signing"))
                }
                // パスワードの保存 (既定はオフ)。使えない端末では理由を一言
                if (canSavePassword) {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Checkbox(checked = savePassword, onCheckedChange = { savePassword = it }, enabled = !busy, modifier = Modifier.testTag("smb_save_password"))
                        Column(Modifier.weight(1f)) {
                            Text(stringResource(R.string.smb_save_password), style = MaterialTheme.typography.bodyMedium)
                            Text(stringResource(R.string.smb_save_password_desc), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                    }
                    if (hasSavedPassword && savePassword) Text(stringResource(R.string.smb_save_password_kept), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                } else {
                    Text(
                        stringResource(when (vaultState) {
                            VaultSupport.OsTooOld -> R.string.unlock_remember_unavailable_os
                            VaultSupport.NoCredential -> R.string.unlock_remember_unavailable_no_lock
                            else -> R.string.unlock_remember_unavailable_other
                        }),
                        style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant, modifier = Modifier.testTag("smb_save_unavailable"),
                    )
                }
                Text(stringResource(R.string.smb_privacy), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                Row(horizontalArrangement = Arrangement.spacedBy(8.dp), verticalAlignment = Alignment.CenterVertically) {
                    OutlinedButton(onClick = { test() }, enabled = !busy, modifier = Modifier.weight(1f).testTag("smb_test")) { Text(stringResource(R.string.smb_test)) }
                    if (testing) TextButton(onClick = { attempt?.cancel() }, modifier = Modifier.testTag("smb_test_cancel")) { Text(stringResource(R.string.smb_cancel_connect)) }
                }
                if (testing) {
                    LinearProgressIndicator(Modifier.fillMaxWidth())
                    Text(stringResource(R.string.smb_testing), style = MaterialTheme.typography.bodySmall)
                }
                testNotice?.let { Banner(it, Modifier.testTag("smb_test_notice")) }
            }
        },
        confirmButton = { TextButton(onClick = { save() }, enabled = !busy, modifier = Modifier.testTag("smb_save")) { Text(stringResource(R.string.smb_save)) } },
        dismissButton = { TextButton(onClick = onDismiss, enabled = !saving, modifier = Modifier.testTag("smb_edit_cancel")) { Text(stringResource(R.string.common_cancel)) } },
    )
}

/** マウント: パスワード (保存してあれば生体認証 / 画面ロックで取り出す) と書き込みの許可を聞いて、接続する。キャンセルでは何も変わらない。 */
@Composable
private fun SmbMountDialog(conn: SmbConnection, hasSavedPassword: Boolean, onDismiss: () -> Unit, onDone: (Notice) -> Unit) {
    val ctx = LocalContext.current.applicationContext
    val scope = rememberUiScope()
    var password by remember { mutableStateOf("") }
    var showPassword by remember { mutableStateOf(false) }
    var write by remember { mutableStateOf(false) }   // 書き込みは毎回明示的に許可する (保存しない)
    var saved by remember { mutableStateOf(hasSavedPassword) }
    var busy by remember { mutableStateOf(false) }
    var error by remember { mutableStateOf<String?>(null) }
    var attempt by remember { mutableStateOf<SmbCancel?>(null) }

    OnStopEffect { password = ""; showPassword = false }
    DisposableEffect(Unit) { onDispose { password = ""; attempt?.cancel() } }

    /** 使うパスワード (配列。呼び出し側が渡す先でゼロにする)。取り出せなければ null (error を設定済み / 取り消し) */
    suspend fun obtainPassword(): CharArray? {
        if (conn.isGuest) return CharArray(0)
        if (saved) {
            return when (val r = withContext(Dispatchers.IO) { SmbController.recallPassword(ctx, conn) }) {
                is SmbRecalled.Password -> r.chars
                SmbRecalled.Cancelled -> null
                SmbRecalled.Invalidated -> { saved = false; error = ctx.getString(R.string.smb_password_invalidated); null }
                SmbRecalled.NotSaved -> { saved = false; error = ctx.getString(R.string.smb_password_not_saved); null }
                is SmbRecalled.Failed -> { error = ctx.getString(R.string.smb_password_recall_failed, r.message); null }
            }
        }
        val pw = password.toCharArray()
        password = ""
        return pw
    }

    fun mount() {
        busy = true; error = null
        scope.launch {
            val pw = obtainPassword()
            if (pw == null) { busy = false; return@launch }
            val a = SmbCancel(); attempt = a
            val r = withContext(Dispatchers.IO) { try { runCatching { SmbController.mount(ctx, conn, pw, write, a) } } finally { SmbSecrets.wipe(pw) } }
            busy = false; attempt = null
            r.fold(
                onSuccess = { m ->
                    val modeText = ctx.getString(if (m.writable) R.string.smb_notice_mounted_write else R.string.smb_notice_mounted_read_only)
                    val forced = write && !m.writable
                    onDone(Notice(
                        if (forced) Severity.Warning else Severity.Success,
                        ctx.getString(R.string.smb_notice_mounted, conn.name),
                        ctx.getString(R.string.smb_notice_mounted_detail, conn.name, modeText) + (if (forced) "\n" + ctx.getString(R.string.smb_notice_mounted_forced_read_only) else ""),
                    ))
                },
                onFailure = { error = SmbUiText.error(ctx, it, conn) },
            )
        }
    }

    val canMount = !busy && (conn.isGuest || saved || password.isNotEmpty())
    AlertDialog(
        onDismissRequest = { if (!busy) onDismiss() },
        modifier = Modifier.padding(horizontal = SMB_DIALOG_MARGIN).imePadding().testTag("smb_mount_dialog"),
        properties = SMB_DIALOG,
        title = { Text(stringResource(R.string.smb_mount_title, conn.name)) },
        text = {
            Column(Modifier.verticalScroll(rememberScrollState()), verticalArrangement = Arrangement.spacedBy(8.dp)) {
                Text(conn.target(), style = MaterialTheme.typography.titleSmall)
                error?.let { Banner(Notice(Severity.Error, it), Modifier.testTag("smb_mount_error")) }
                if (conn.isGuest) {
                    Text(stringResource(R.string.smb_mount_guest), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                } else if (saved) {
                    Text(stringResource(R.string.smb_mount_saved_password), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    TextButton(
                        onClick = { scope.launch { withContext(Dispatchers.IO) { SmbController.forgetPassword(ctx, conn) }; saved = false } },
                        enabled = !busy, modifier = Modifier.testTag("smb_forget_password"),
                    ) { Text(stringResource(R.string.smb_forget_password)) }
                } else {
                    OutlinedTextField(
                        value = password, onValueChange = { password = it }, enabled = !busy, singleLine = true, label = { Text(stringResource(R.string.smb_field_password)) },
                        visualTransformation = if (showPassword) VisualTransformation.None else PasswordVisualTransformation(),
                        keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Password), modifier = Modifier.fillMaxWidth().testTag("smb_mount_password"),
                    )
                    TextButton(onClick = { showPassword = !showPassword }, enabled = !busy, modifier = Modifier.testTag("smb_mount_show_password")) {
                        Text(stringResource(if (showPassword) R.string.smb_password_hide else R.string.smb_password_show))
                    }
                }
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Column(Modifier.weight(1f)) {
                        Text(stringResource(R.string.smb_mount_write), style = MaterialTheme.typography.bodyLarge)
                        Text(stringResource(R.string.smb_mount_write_desc), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                    }
                    Switch(checked = write, onCheckedChange = { write = it }, enabled = !busy, modifier = Modifier.testTag("smb_mount_write"))
                }
                Text(stringResource(R.string.smb_privacy), style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
                if (busy) {
                    LinearProgressIndicator(Modifier.fillMaxWidth())
                    Text(stringResource(R.string.smb_mounting), style = MaterialTheme.typography.bodySmall)
                    TextButton(onClick = { attempt?.cancel() }, modifier = Modifier.testTag("smb_mount_cancel_connect")) { Text(stringResource(R.string.smb_cancel_mount)) }
                }
            }
        },
        confirmButton = { TextButton(onClick = { mount() }, enabled = canMount, modifier = Modifier.testTag("smb_mount_ok")) { Text(stringResource(R.string.smb_mount_button)) } },
        dismissButton = { TextButton(onClick = { attempt?.cancel(); onDismiss() }, modifier = Modifier.testTag("smb_mount_cancel")) { Text(stringResource(R.string.common_cancel)) } },
    )
}
