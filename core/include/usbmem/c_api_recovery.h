/* SPDX-License-Identifier: Apache-2.0 */
/* データ救出 (ddrescue 方式) とバックアップ / 復元 (Partclone 方式) の C 境界 API。設計は docs/recovery-backup.md。
 * 戻り値 0 = 成功、それ以外は usbmem::Errc (c_api.h と同じ数値)。結果は JSON 文字列 (usbmem_free で解放)。
 * 長い処理 (run / backup / apply) は呼び出したスレッドをふさぐ。別スレッドから usbmem_job_cancel / progress_json を呼べる。 */
#ifndef USBMEM_C_API_RECOVERY_H
#define USBMEM_C_API_RECOVERY_H
#include <stddef.h>

#include "usbmem/c_api.h"
#ifdef __cplusplus
extern "C" {
#endif

/* ---- 実行中の仕事 (取り消し・一時停止・進捗) */
typedef struct usbmem_job usbmem_job;
usbmem_job* usbmem_job_new(void);
void usbmem_job_cancel(usbmem_job* j);
void usbmem_job_pause(usbmem_job* j, int paused); /* 救出だけ */
/* 直近の進捗 JSON: {"phase":..,"pos":..,"size":..,"finished":..,"nonTried":..,"nonTrimmed":..,"nonScraped":..,"bad":..,
 *                  "readOps":..,"readErrors":..,"bytesCopied":..,"msSinceLastGood":..,"done":..,"total":..} */
int usbmem_job_progress_json(usbmem_job* j, char** out_json);
void usbmem_job_free(usbmem_job* j);

/* ---- データ救出 */
typedef struct usbmem_rescue_options {
  unsigned long long start, length;       /* 入力の範囲 (length 0 = 末尾まで) */
  unsigned long long out_offset;          /* 入力位置 start に対応する出力位置 */
  unsigned sector_size;                   /* 0 = 自動 */
  unsigned cluster_sectors;               /* 0 = 64 */
  unsigned retries;                       /* 不良セクタの再試行回数 */
  int no_trim, no_scrape, no_skip;
  unsigned read_timeout_ms;
  /* 安全弁 (0 = 無効) */
  unsigned max_consecutive_errors;
  unsigned long long max_errors;
  unsigned error_window_ms, max_errors_in_window;
  unsigned long long no_progress_timeout_ms;
  unsigned pause_between_reads_ms;
} usbmem_rescue_options;

/* in を読み、out へ複製する。mapfile_path が既にあれば読んで続きから行い (壊れていれば Corrupt で断る)、なければ作る。
 * 進行中は mapfile を保存し続ける (一時ファイル + rename)。mapfile_path が NULL なら保存しない (中断すると続きから行えない)。
 * in と out が同じデバイスなら InvalidArgument。out が読み取り専用なら ReadOnly。
 * *out_result_json: {"stop":"finished|cancelled|safety-abort|input-gone|output-error|mapfile-error|bad-argument","code":..,"message":..,
 *                    "errc":N,"finished":..,"nonTried":..,"nonTrimmed":..,"nonScraped":..,"bad":..,"readOps":..,"readErrors":..,"bytesCopied":..} */
int usbmem_rescue_run(usbmem_device* in, usbmem_device* out, const char* mapfile_path, const usbmem_rescue_options* opt, usbmem_job* job, char** out_result_json);

/* ---- バックアップ (Partclone 形式) */
/* src (ファイルシステムの先頭から始まるデバイス。パーティションなら usbmem_device_sub_range で切り出す) の使用中ブロックだけを
 * image_path に書く (既存ファイルは置き換える)。lz4 != 0 なら LZ4 フレームで圧縮する。full_if_unclean != 0 なら、割り当て情報が信用できない
 * (ext の journal 未再生・NTFS の dirty) ときに全ブロックを複製する (0 なら断る)。src には書き込まない。
 * *out_result_json: {"ok":bool,"code":..,"message":..,"errc":N,"fs":..,"blockSize":..,"totalBlocks":..,"usedBlocks":..,"fullCopy":bool,
 *                    "imageBytes":..,"imageSha256":..,"warnings":[..]} 失敗時は不完全な image_path を削除する。 */
int usbmem_backup_run(usbmem_device* src, const char* image_path, int lz4, int full_if_unclean, usbmem_job* job, char** out_result_json);

/* 画像を調べる (verify != 0 なら全体のチェックサムも確かめる)。lz4 は自動判別。 */
int usbmem_backup_inspect_json(const char* image_path, int verify, char** out_json);

/* ---- 復元: 計画 → 確認トークン → 適用 */
typedef struct usbmem_restore_plan usbmem_restore_plan;
/* 計画を作る (何も書かない)。*out_plan_json: {"ok":bool,"code":..,"message":..,"fs":..,"blockSize":..,"usedBlocks":..,"bytesToWrite":..,
 *   "targetSize":..,"targetExistingFs":..,"ntfsBackupBoot":bool,"verified":bool,"imageSha256":..,"planHash":..,"confirmPhrase":"RESTORE xxxxxx","warnings":[..]}
 * 計画が ok でなくても *out_plan と JSON は返る (理由を見せるため)。apply は ok でない計画を断る。 */
int usbmem_restore_plan_new(const char* image_path, usbmem_device* target, int target_in_use, int verify_all, usbmem_restore_plan** out_plan, char** out_plan_json);
/* 利用者が打った文字列から確認トークンを作る (confirmPhrase と一致していなければ apply が断る)。 */
int usbmem_restore_token(usbmem_restore_plan* plan, const char* typed_phrase, char** out_token);
/* 適用。target は計画のときと同じものであること。*out_result_json: {"ok":bool,"code":..,"message":..,"errc":N,"blocksWritten":..,"verified":bool,"targetTouched":bool} */
int usbmem_restore_apply(usbmem_restore_plan* plan, const char* image_path, usbmem_device* target, const char* token, int target_in_use, usbmem_job* job, char** out_result_json);
void usbmem_restore_plan_free(usbmem_restore_plan* plan);

#ifdef __cplusplus
}
#endif
#endif
