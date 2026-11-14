/* SPDX-License-Identifier: Apache-2.0 */
/* 暗号ボリューム (BitLocker / LUKS1 / LUKS2) を復号する C 境界 API (既定は読み取り専用。書き込みは USBMEM_TRANSFORM_WRITABLE で明示したときだけ)。docs/crypto-volumes.md
 * 戻り値 0 = 成功、それ以外は usbmem::Errc (usbmem_errc_name で名前)。AuthFailed (12) = 資格情報違い。
 * 資格情報のバッファは呼び出し側のもの: 関数内でコピーして使い、コピーはゼロ化する。呼び出し側は戻ったら自分のバッファをゼロ化すること。 */
#ifndef USBMEM_C_API_TRANSFORM_H
#define USBMEM_C_API_TRANSFORM_H
#include <stddef.h>

#include "usbmem/c_api.h"
#ifdef __cplusplus
extern "C" {
#endif

enum { USBMEM_CRYPTO_AUTO = 0, USBMEM_CRYPTO_BITLOCKER = 1, USBMEM_CRYPTO_LUKS = 2 };
/* flags */
enum {
  USBMEM_TRANSFORM_ALLOW_PARTIAL = 1, /* BitLocker: 変換途中のボリュームを、変換済み領域だけ復号して開く (読み取り専用) */
  USBMEM_TRANSFORM_WRITABLE = 2       /* 書き込み可で開くことを明示的に要求する。既定 (0) は読み取り専用。ビルドが USBMEM_CRYPT_WRITE でない / 構成が
                                         書き込み非対応 / 下位が読み取り専用のときは、解錠は成功して読み取り専用で開く (JSON の "writable":false と "write_note")。
                                         書き込み可かどうかは usbmem_is_read_only 等で返ったデバイスにも問い合わせられる */
};

/* 資格情報なしでヘッダを調べる。暗号ボリュームでなければ Errc::Unsupported (9) を返し *out_json は NULL。
 * *out_json (UTF-8 JSON, usbmem_free で解放): {"kind":1|2,"format":..,"cipher":..,"supported":bool,"password":bool,"recovery":bool,
 * "key_file":bool,"no_secret":bool,"tpm_only":bool,"in_progress":bool,"write_supported":bool,"writable":bool,"note":..,"write_note":..}。
 * 秘密は含まない。probe では writable は常に false (open の結果だけ)。write_supported = この構成・ビルドなら書き込み可で開ける。 */
int usbmem_transform_probe(usbmem_device* dev, char** out_json);

/* 解錠して復号済みデバイス (既定は読み取り専用) を *out に返す (usbmem_close で閉じる。dev は閉じない: dev を先に閉じないこと)。
 * password: UTF-8 パスワード/パスフレーズ。recovery_key: BitLocker 回復パスワード (48 桁)。key_file: .BEK / LUKS キーファイルの中身。
 * 使わないものは NULL / 0。out_info_json (NULL 可): probe と同じ形式。失敗時 (AuthFailed を含む) も可能なら返す。 */
int usbmem_transform_open(usbmem_device* dev, int kind, const unsigned char* password, size_t password_len, const unsigned char* recovery_key,
                          size_t recovery_key_len, const unsigned char* key_file, size_t key_file_len, unsigned flags, usbmem_device** out,
                          char** out_info_json);

#ifdef __cplusplus
}
#endif
#endif
