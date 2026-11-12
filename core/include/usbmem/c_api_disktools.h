/* SPDX-License-Identifier: Apache-2.0 */
/* DiskTools の C API (JNI など C 境界向け): 読み取り専用の整合性検査 (fsck 相当) と mkfs。設計は docs/disk-tools.md。
 * 戻り値 0 = 成功、それ以外は usbmem::Errc (c_api.h と同じ数値)。機械可読な詳細コードは usbmem_mkfs_last_error で取る。
 * 文字列は UTF-8。char** で返すものは usbmem_free で解放する (c_api.h)。
 *
 * 検査 (usbmem_fscheck_run) は絶対に書き込まない。読み取り専用で開いたデバイスで使える。見つけた問題は報告するだけで、修復はしない。
 * mkfs の流れ: usbmem_mkfs_open (計画を作る・書かない) → plan_json → ユーザーに差分と確認文字列 (confirmPhrase) を見せる →
 *              usbmem_mkfs_make_token (ユーザーが打った文字列) → usbmem_mkfs_apply。 */
#ifndef USBMEM_C_API_DISKTOOLS_H
#define USBMEM_C_API_DISKTOOLS_H
#include <stddef.h>

#include "usbmem/c_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 長い処理の進捗とキャンセル (検査・mkfs 適用で使う。別スレッドから cancel / progress を呼んでよい) */
typedef struct usbmem_job usbmem_job;
usbmem_job* usbmem_job_new(void);
void usbmem_job_free(usbmem_job* job); /* 処理が終わってから呼ぶ */
void usbmem_job_cancel(usbmem_job* job);
/* phase: 現在の段階名 ("write" / "verify" / "mft" など)。done / total: 段階内の進み具合 (total 0 = 不明) */
void usbmem_job_progress(usbmem_job* job, char* phase, size_t phase_len, unsigned long long* done, unsigned long long* total);

/* ---- 検査 */
/* dev の [offset, offset+length) を検査する (length 0 = デバイスの終わりまで)。
 * fs_name: NULL / "" = 自動判別、"fat12" "fat16" "fat32" "exfat" "ext2" "ext3" "ext4" "ntfs"。
 * max_findings: 0 = 既定 (500)。memory_limit: バイト、0 = 既定 (256 MiB)。job: NULL 可。
 * 出力 JSON: {"fs","verdict":"clean|warnings|errors|unsupported|incomplete","incomplete","unclean","needsJournalRecovery",
 *   "errors","warnings","infos","stats":{...},"findings":[{"severity","code","message","offset"?,"object"?}]}。
 * キャンセルされると Errc::Cancelled を返すが、それまでの報告 (verdict "incomplete") は out_json に入る。 */
int usbmem_fscheck_run(usbmem_device* dev, unsigned long long offset, unsigned long long length, const char* fs_name, unsigned max_findings,
                       unsigned long long memory_limit, usbmem_job* job, char** out_json);
int usbmem_fscheck_supported(const char* fs_name); /* 1 = 検査できる */

/* ---- mkfs (ext2 / exFAT / FAT32 のみ) */
typedef struct usbmem_mkfs usbmem_mkfs;

/* 計画を作る (書かない)。fs_name: "ext2" "exfat" "fat32"。length 0 = デバイスの終わりまで。label: NULL 可 (UTF-8)。
 * cluster_size: 0 = 自動 (ext2 ではブロックサイズ)。sector_size: 0 = デバイスの論理ブロックサイズ。inode_ratio / inode_size: ext2 用で 0 = 既定。
 * in_use != 0: マウント中など (計画が不可になる)。seed: 0 = OS 乱数 (ボリュームシリアル / UUID)。timestamp: 0 = 現在時刻。
 * 計画が作れなくても (サイズ不足・不正なラベルなど) 戻り値は 0 で、plan_json の ok が false になる。I/O エラーだけが非 0。 */
int usbmem_mkfs_open(usbmem_device* dev, const char* fs_name, unsigned long long offset, unsigned long long length, const char* label,
                     unsigned cluster_size, unsigned sector_size, unsigned inode_ratio, unsigned inode_size, int in_use, unsigned long long seed,
                     unsigned long long timestamp, usbmem_mkfs** out);
void usbmem_mkfs_close(usbmem_mkfs* h);
/* 直近の失敗の機械可読コード ("bad-token" / "in-use" / "device-changed" / "busy" / "verify-failed" など) と英語メッセージ。 */
void usbmem_mkfs_last_error(usbmem_mkfs* h, char* code, size_t code_len, char* msg, size_t msg_len);

/* 計画の JSON (ok / problems / changes / layout / stages / requiresForce / confirmPhrase / planHash ...) */
int usbmem_mkfs_plan_json(usbmem_mkfs* h, char** out_json);
/* 確認トークン。typed_phrase はユーザーが打った文字列 (confirmPhrase と一致していなければ apply で拒否される) */
int usbmem_mkfs_make_token(usbmem_mkfs* h, const char* typed_phrase, char** out_token);
/* 計画を適用する。backup_path: 上書きする領域の元の内容を保存するファイル (NULL なら直近のバックアップを usbmem_mkfs_last_backup で取れるだけ。
 * 保存できなければ何も書かずに失敗する)。in_use != 0: 適用の直前の状態 (マウント中なら拒否)。job: NULL 可。
 * 結果は JSON (ok / code / message / bytesWritten / rolledBack / verified / checkVerdict / verifyProblems)。
 * 書き込み後に読み戻しと検査をして、verified が true のときだけ「できた」とみなす。 */
int usbmem_mkfs_apply(usbmem_mkfs* h, const char* confirm_token, const char* backup_path, unsigned long long timestamp, int in_use, usbmem_job* job,
                      char** out_result_json);

unsigned long long usbmem_mkfs_min_size(const char* fs_name, unsigned sector_size); /* 0 = 非対応 */
int usbmem_mkfs_last_backup(usbmem_mkfs* h, unsigned char** out, size_t* out_len); /* usbmem_free */
int usbmem_mkfs_backup_info_json(const unsigned char* data, size_t len, char** out_json); /* 検査のみ */
/* バックアップを書き戻す (dev は書き込み可で開いたもの)。ゼロ埋めされただけの大きな領域は戻らない。 */
int usbmem_mkfs_restore_backup(usbmem_device* dev, const unsigned char* data, size_t len);

#ifdef __cplusplus
}
#endif
#endif
