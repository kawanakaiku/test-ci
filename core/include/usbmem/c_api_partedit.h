/* SPDX-License-Identifier: Apache-2.0 */
/* パーティション編集の C API (JNI など C 境界向け)。設計は docs/partition-edit.md。
 * 戻り値 0 = 成功、それ以外は usbmem::Errc (c_api.h と同じ数値)。機械可読な詳細コードは usbmem_partedit_last_error で取る。
 * 文字列は UTF-8。char** で返すものは usbmem_free で解放する (c_api.h)。
 * 編集の流れ: open → 操作 (create / remove / ...) → plan_json → ユーザーに差分と確認文字列を見せる →
 *             make_token (ユーザーが打った文字列) → apply。計画後に操作を足すと計画は破棄され、apply は "no-plan" で拒否される。 */
#ifndef USBMEM_C_API_PARTEDIT_H
#define USBMEM_C_API_PARTEDIT_H
#include <stddef.h>

#include "usbmem/c_api.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct usbmem_partedit usbmem_partedit;

/* dev: usbmem_open_* で開いたデバイス (書き込み可で開くと編集できる。読み取り専用でも計画・バックアップ・エクスポートは使える)。
 * sector_size: 0 = 自動。in_use != 0: マウント中などで適用を拒否する。rng_seed: 0 = OS 乱数。
 * 同じデバイスに書き込み可のエディタは 1 つだけ (2 つ目は Errc::InvalidArgument、コード "busy")。 */
int usbmem_partedit_open(usbmem_device* dev, unsigned sector_size, int in_use, unsigned long long rng_seed, usbmem_partedit** out);
void usbmem_partedit_close(usbmem_partedit* h);
void usbmem_partedit_set_in_use(usbmem_partedit* h, int in_use);
/* 直近の失敗の機械可読コード (例 "busy" / "overlap" / "bad-token") と英語メッセージ。h == NULL なら直近の open の失敗 (スレッドごと)。 */
void usbmem_partedit_last_error(usbmem_partedit* h, char* code, size_t code_len, char* msg, size_t msg_len);

/* ディスク上の現在のテーブル (partition_table_to_json) と GPT の詳細検査 (suggestedRepairs を含む) */
int usbmem_partedit_table_json(usbmem_partedit* h, char** out_json);
int usbmem_partedit_gpt_json(usbmem_partedit* h, char** out_json);

/* ---- 操作。handle は table_json の partitions[].index (既存) か create が返す値 */
/* role: 0 自動 / 1 主 / 2 拡張 / 3 論理 (MBR のみ)。start / count: セクタ。0 = 自動配置 / 空きの最後まで。
 * type_guid / unique_guid / name は GPT 用 (NULL 可。NULL なら Linux filesystem / 新規生成 / 空)。mbr_type: MBR 用 (0x83 など) */
int usbmem_partedit_create(usbmem_partedit* h, int role, unsigned long long start, unsigned long long count, int mbr_type,
                           const char* type_guid, const char* unique_guid, const char* name, unsigned long long attrs, int bootable,
                           unsigned* out_handle);
int usbmem_partedit_remove(usbmem_partedit* h, unsigned handle);
int usbmem_partedit_resize(usbmem_partedit* h, unsigned handle, unsigned long long new_count);
int usbmem_partedit_move(usbmem_partedit* h, unsigned handle, unsigned long long new_start);
int usbmem_partedit_set_mbr_type(usbmem_partedit* h, unsigned handle, int type);
int usbmem_partedit_set_gpt_type(usbmem_partedit* h, unsigned handle, const char* guid);
int usbmem_partedit_set_name(usbmem_partedit* h, unsigned handle, const char* utf8);
int usbmem_partedit_set_attrs(usbmem_partedit* h, unsigned handle, unsigned long long attrs);
int usbmem_partedit_set_bootable(usbmem_partedit* h, unsigned handle, int bootable);
int usbmem_partedit_set_unique_guid(usbmem_partedit* h, unsigned handle, const char* guid);
int usbmem_partedit_set_disk_id(usbmem_partedit* h, const char* id);
/* kind: 1 = MBR / 2 = GPT。overwrite_filesystem != 0: パーティション無しのファイルシステムのブートセクタを壊すことを明示 */
int usbmem_partedit_new_table(usbmem_partedit* h, int kind, unsigned gpt_entries, int overwrite_filesystem);
/* kind: 1 copy-primary-to-backup / 2 copy-backup-to-primary / 3 move-backup-to-end / 4 rebuild-protective-mbr。最初の操作としてのみ */
int usbmem_partedit_repair_gpt(usbmem_partedit* h, int kind);
void usbmem_partedit_clear_ops(usbmem_partedit* h);

/* ---- 計画と適用 */
/* 計画を作り JSON で返す (ok / problems / changes / stages / confirmPhrase / requiresForce ...)。書き込みはしない */
int usbmem_partedit_plan_json(usbmem_partedit* h, int include_sectors, char** out_json);
/* 直近の計画に対する確認トークン。typed_phrase はユーザーが打った文字列 (confirmPhrase と一致していなければ apply で拒否される) */
int usbmem_partedit_make_token(usbmem_partedit* h, const char* typed_phrase, char** out_token);
/* 直近の計画を適用する。backup_path: 書き込み前の表を保存するファイル (NULL なら保存しない。ただしその場合は
 * 直近のバックアップ usbmem_partedit_last_backup だけが残る)。結果は JSON (ok / code / message / sectorsWritten / rolledBack / verified)。
 * 書き込み前にバックアップを必ず作る (保存できなければ何も書かずに失敗) */
int usbmem_partedit_apply(usbmem_partedit* h, const char* confirm_token, const char* backup_path, unsigned long long timestamp,
                          char** out_result_json);

/* ---- バックアップ / 復元 / エクスポート */
int usbmem_partedit_backup(usbmem_partedit* h, unsigned long long timestamp, unsigned char** out, size_t* out_len);  /* usbmem_free */
int usbmem_partedit_last_backup(usbmem_partedit* h, unsigned char** out, size_t* out_len);  /* 直近の apply が書き込み前に作った表 */
int usbmem_partedit_backup_info_json(const unsigned char* data, size_t len, char** out_json);  /* 検査のみ (version / sectorSize / diskId ...) */
int usbmem_partedit_plan_restore(usbmem_partedit* h, const unsigned char* data, size_t len, char** out_plan_json);  /* 計画を保持。apply で適用 */
int usbmem_partedit_export_sfdisk(usbmem_partedit* h, const char* device_name, char** out_text);

#ifdef __cplusplus
}
#endif
#endif
