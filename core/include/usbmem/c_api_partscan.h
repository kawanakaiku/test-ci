/* SPDX-License-Identifier: Apache-2.0 */
/* パーティション探索 / 失われた表の復元案 / ブートセクタの検査と修復の C API (JNI など C 境界向け)。設計は docs/partition-scan.md。
 * 戻り値 0 = 成功、それ以外は usbmem::Errc (c_api.h と同じ数値)。機械可読な詳細コードは *_last_error で取る。
 * 文字列は UTF-8。char** で返すものは usbmem_free で解放する (c_api.h)。
 *
 * 探索 (usbmem_partscan_run) と検査 (usbmem_bootfix_open) は絶対に書き込まない。読み取り専用で開いたデバイスで使える。
 * 書き込みは必ず 計画 → 確認トークン → 適用 の 3 段:
 *   - 表の復元: usbmem_partscan_queue_proposal で案を編集セッション (usbmem_partedit) に積み、あとは usbmem_partedit_plan_json / make_token / apply
 *     (書き込み前にバックアップ・読み戻し検証つき。docs/partition-edit.md)。
 *   - ブートセクタの修復: usbmem_bootfix_plan_json → usbmem_bootfix_make_token → usbmem_bootfix_apply (書き込み前にバックアップ・読み戻し検証つき)。 */
#ifndef USBMEM_C_API_PARTSCAN_H
#define USBMEM_C_API_PARTSCAN_H
#include <stddef.h>

#include "usbmem/c_api.h"
#include "usbmem/c_api_partedit.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 長い処理の進捗とキャンセル (別スレッドから cancel / progress を呼んでよい。処理が終わってから free) */
typedef struct usbmem_pjob usbmem_pjob;
usbmem_pjob* usbmem_pjob_new(void);
void usbmem_pjob_free(usbmem_pjob* job);
void usbmem_pjob_cancel(usbmem_pjob* job);
/* phase: "table" / "scan" / "verify" (探索)、"backup" / "write" / "verify" (修復の適用)。done / total: 段階内の進み具合 (total 0 = 不明)。
 * 探索の scan 段階の単位: quick = 調べた位置の数、full = 読んだバイト数 */
void usbmem_pjob_progress(usbmem_pjob* job, char* phase, size_t phase_len, unsigned long long* done, unsigned long long* total);

/* ---- パーティション探索 (読み取り専用) */
typedef struct usbmem_partscan usbmem_partscan;
/* level: 0 = quick (典型的な位置だけ) / 1 = full (全セクタ)。sector_size: 0 = 自動。min_confidence: 0 = 既定 (40)。job: NULL 可。
 * キャンセルされると Errc::Cancelled を返すが、それまでの結果を持つハンドルを *out に返す (cancelled: true)。 */
int usbmem_partscan_run(usbmem_device* dev, unsigned sector_size, int level, int min_confidence, usbmem_pjob* job, usbmem_partscan** out);
void usbmem_partscan_close(usbmem_partscan* h);
/* 結果: {"sectorSize","diskBytes","level","positionsProbed","bytesRead","readErrors","cancelled","table":{...},"tableEntriesWithoutFs":[...],
 *   "candidates":[{"index","fs","variant","startLba","sectorCount","startBytes","sizeBytes","sizeKnown","sizeMinimum","fitsDevice","confidence",
 *   "primaryOk","backupOk","relation":"not-in-table|match|overlap|contained","tableIndex","nestedIn","notes":[...]}]} (64 ビット値は文字列) */
int usbmem_partscan_result_json(usbmem_partscan* h, char** out_json);
/* 復元案 (信頼度順。読み取り専用のビューで PartitionEditor の計画を作って検証済み): [{"index","id","title","table","action","confidence","planOk",
 *   "confirmPhrase","notes","problems","changes","partitions":[...]}] */
int usbmem_partscan_proposals_json(usbmem_partscan* h, char** out_json);
/* 案 (proposals_json の index) の操作を編集セッションに積む (既存の操作は捨てる)。書き込みはしない。あとは usbmem_partedit_plan_json → make_token → apply。
 * ed は同じデバイスで開いた usbmem_partedit。 */
int usbmem_partscan_queue_proposal(usbmem_partscan* h, int proposal_index, usbmem_partedit* ed);

/* ---- ブートセクタの検査と修復 */
typedef struct usbmem_bootfix usbmem_bootfix;
/* dev の [offset, offset+length) (length 0 = デバイスの終わりまで) を検査する (読み取り専用)。fs_name: NULL / "" = 自動判別、
 * "fat12" "fat16" "fat32" "exfat" "ext2" "ext3" "ext4" "ntfs"。壊れていても戻り値は 0 (結果の verdict / findings で判断する)。I/O エラーだけが非 0。 */
int usbmem_bootfix_open(usbmem_device* dev, unsigned long long offset, unsigned long long length, const char* fs_name, usbmem_pjob* job, usbmem_bootfix** out);
void usbmem_bootfix_close(usbmem_bootfix* h);
void usbmem_bootfix_last_error(usbmem_bootfix* h, char* code, size_t code_len, char* msg, size_t msg_len);
/* 検査結果: {"fs","recognized","verdict":"ok|warnings|repairable|damaged|unrecognized","bootOk","hasBackupStructure","backupOk","findings":[...],
 *   "repairs":[{"id","description","source"}],"details":{...},"proposedBpb":{...},"extBackupBlocks":[...]} */
int usbmem_bootfix_inspection_json(usbmem_bootfix* h, char** out_json);
/* 修復の計画 (書かない)。repair_id: inspection の repairs[].id。source: ext の予備スーパーブロックのブロック番号 (0 = 最初のもの)。
 * 計画が作れなくても (当てはまらない修復など) 戻り値は 0 で、JSON の ok が false になる。 */
int usbmem_bootfix_plan_json(usbmem_bootfix* h, const char* repair_id, unsigned long long source, char** out_json);
/* 確認トークン。typed_phrase はユーザーが打った文字列 (計画の confirmPhrase "REPAIR xxxxxx" と一致していなければ apply で拒否される) */
int usbmem_bootfix_make_token(usbmem_bootfix* h, const char* typed_phrase, char** out_token);
/* 直近の計画を適用する。backup_path: 書き込み前の内容を保存するファイル (保存して fsync できなければ何も書かずに失敗する。NULL なら保存しない
 * (直近のバックアップは usbmem_bootfix_last_backup で取れる))。in_use != 0: マウント中など (拒否)。job: NULL 可。
 * 結果 JSON: {"ok","code","message","writesDone","rolledBack","verified","verifyFindings":[...]} */
int usbmem_bootfix_apply(usbmem_bootfix* h, const char* confirm_token, const char* backup_path, unsigned long long timestamp, int in_use, usbmem_pjob* job,
                         char** out_result_json);
int usbmem_bootfix_last_backup(usbmem_bootfix* h, unsigned char** out, size_t* out_len); /* usbmem_free */
int usbmem_bootfix_backup_info_json(const unsigned char* data, size_t len, char** out_json); /* 検査のみ */
/* バックアップを書き戻す (dev は書き込み可で開いたもの) */
int usbmem_bootfix_restore_backup(usbmem_device* dev, const unsigned char* data, size_t len);

#ifdef __cplusplus
}
#endif
#endif
