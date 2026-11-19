/* SPDX-License-Identifier: GPL-2.0-or-later */
/* マウント時に分かった「ボリュームの状態」のビット (C / C++ 共通)。UI が原因別の案内 (Windows で安全に取り外してください、など) を出すための事実だけを立てる。
 * 読み取れなかったもの・推測になるものは立てない。ログの再生もダーティフラグの書き換えもこの層の仕事ではない (docs/dirty-volumes.md)。 */
#ifndef USBMEM_VOL_STATE_H
#define USBMEM_VOL_STATE_H

/* ダーティフラグ: 前回きれいに取り外されていない (NTFS: $Volume の VOLUME_IS_DIRTY / exFAT: VolumeDirty / FAT: ブートセクタのダーティビット・FAT[1] のクリーン bit)。
 * NTFS の「高速スタートアップ」「取り外し忘れ」でも立つ。マウント時の値 (書き込みマウントが後から立てるものは含まない)。 */
#define USBMEM_VOL_DIRTY 1u
/* NTFS: $LogFile の restart area に「クリーン」の印がなく、未再生のログが残っている (Windows が書き込み途中のまま取り外された / 休止された)。 */
#define USBMEM_VOL_LOG_UNCLEAN 2u
/* NTFS: ルートに hiberfil.sys があり、Windows が休止状態 (高速スタートアップを含む) のまま。上流ドライバの判定そのもの (判定できなかったときも立つ)。 */
#define USBMEM_VOL_HIBERNATED 4u
/* exFAT の MediaFailure / FAT のメディアエラー bit: 表面検査が必要と記録されている。 */
#define USBMEM_VOL_MEDIA_ERROR 8u
/* NTFS: ダーティフラグだけ (ログは静止) のボリュームに書き込み可を要求されたが、mount_rw の allow_dirty = false だったので、書き込みマウントをせず読み取り専用のまま返した
 * (writable() == false)。呼び出し側 (UI) が利用者に確認して、「読み取り専用で開く」か、allow_dirty = true で開き直す (書き込み可)。ボリュームの事実ではなく、この層の保留の合図。 */
#define USBMEM_VOL_CONFIRM_WRITE 16u

/* NTFS: $MFT の先頭 4 レコード (0-3) のどれかが torn write などで壊れていたため、$MFTMirr のコピーで読み出している (読み取り専用で開く。書き込みは許さない)。
 * コピーは最後に同期された時点のもので、新しい更新が見えないことがある。docs/ntfs-mftmirr.md */
#define USBMEM_VOL_MFT_FROM_MIRROR 32u

#endif
