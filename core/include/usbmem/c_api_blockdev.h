/* SPDX-License-Identifier: Apache-2.0 */
/* ディスクイメージ (QCOW2/VHD/VHDX/VMDK) / COW オーバーレイ / NBD クライアントの C 境界。c_api.h の usbmem_device をそのまま使う。
 * 戻り値 0 = 成功、それ以外は usbmem::Errc。文字列の出力は usbmem_free で解放する。
 * 閉じ方: イメージ / オーバーレイ / NBD のハンドルを usbmem_close すると、下位のファイルも閉じる (後から下位を usbmem_close しても無害)。 */
#ifndef USBMEM_C_API_BLOCKDEV_H
#define USBMEM_C_API_BLOCKDEV_H
#include "usbmem/c_api.h"
#ifdef __cplusplus
extern "C" {
#endif

/* イメージが参照する別ファイル (バッキング / 親 / VMDK のエクステント) を呼び出し側が開く。
 * kind: 0 = バッキング(親), 1 = エクステント。name はイメージ内の名前 (UTF-8, '/' 区切りに正規化済み。信用しないこと: 絶対パスや ".." を含みうる)。
 * format_hint: "qcow2" / "raw" / "vmdk" / "vpc" / "vhdx" / ""。成功したら *out に読み取り専用デバイスを返し 0 を返す。 */
typedef int (*usbmem_image_resolve_fn)(void* user, const char* name, int kind, const char* format_hint, usbmem_device** out);

/* file を QCOW2 / VHD / VHDX / VMDK / RAW のいずれかとして自動判別して開く (読み取り専用)。RAW のときは file 自身を返す。
 * resolve == NULL なら、別ファイルを要するイメージは Unsupported。
 * out_info_json (NULL 可): {"format","subformat","virtual_size","logical_block_size","has_parent","parent_name","compressed","content_id","detail"}。失敗時も理由 (detail) を返す。 */
int usbmem_image_open(usbmem_device* file, usbmem_image_resolve_fn resolve, void* user, usbmem_device** out, char** out_info_json);
/* パスから開く (ホスト/テスト用)。バッキングは、イメージと同じディレクトリからの相対名のみ解決する (allow_unsafe_paths != 0 で絶対パス・".." も許す)。 */
int usbmem_image_open_path(const char* path, int allow_unsafe_paths, usbmem_device** out, char** out_info_json);
/* 形式名 ("raw"/"qcow2"/"vhd"/"vhdx"/"vmdk") を buf へ。 */
int usbmem_image_detect(usbmem_device* file, char* buf, size_t len);

/* COW オーバーレイ: base (読み取り専用で十分) の上に書き込みを溜める。base には決して書かない。
 * log_path == NULL ならメモリ上。指定すればそのファイルに追記ログとして保存し、既存ならそこから再開する。block_size 0 = 4096。max_bytes 0 = 無制限。 */
int usbmem_overlay_create(usbmem_device* base, const char* log_path, unsigned int block_size, unsigned long long max_bytes, usbmem_device** out);
unsigned long long usbmem_overlay_dirty_bytes(usbmem_device* overlay);
/* 溜めた書き込みを target (書き込み可能・十分な大きさ) へ書き戻して flush する。clear_after != 0 で成功後に破棄。 */
int usbmem_overlay_commit(usbmem_device* overlay, usbmem_device* target, int clear_after);
int usbmem_overlay_discard(usbmem_device* overlay);

/* NBD クライアント (fixed newstyle, 構造化応答, TLS なし)。unix_socket != NULL ならそれを使う (host/port は無視)。
 * Android アプリがネットワーク越しに使うには INTERNET 権限が必要 (localhost の UNIX ソケットは不要)。
 * 0 を指定したタイムアウトは既定値 (接続 10 s / I/O 30 s)。out_detail (NULL 可) に失敗理由 (英語)。 */
int usbmem_nbd_open(const char* host, int port, const char* unix_socket, const char* export_name, int read_only,
                    unsigned int connect_timeout_ms, unsigned int io_timeout_ms, usbmem_device** out, char** out_detail);
/* エクスポート名の一覧 (JSON 配列の文字列) */
int usbmem_nbd_list_exports_json(const char* host, int port, const char* unix_socket, char** out_json, char** out_detail);
/* 統計 JSON: {"size","read_only","structured","min_block","max_block","flags","reconnects"}。NBD でなければ Unsupported */
int usbmem_nbd_info_json(usbmem_device* dev, char** out_json);

#ifdef __cplusplus
}
#endif
#endif
