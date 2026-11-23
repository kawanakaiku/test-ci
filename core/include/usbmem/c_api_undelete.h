/* SPDX-License-Identifier: Apache-2.0 */
/* 削除ファイルの一覧と救出 / カービング (PhotoRec 相当) の C 境界 API (JNI など C 境界向け)。設計は docs/undelete-carving.md。
 * 戻り値 0 = 成功、それ以外は usbmem::Errc (c_api.h と同じ数値)。結果は UTF-8 JSON の文字列 (usbmem_free で解放)。
 * 長い処理 (scan / run / extract) は呼び出したスレッドをふさぐので、別スレッドから usbmem_ujob_cancel / usbmem_ujob_progress を呼ぶ。
 *
 * どちらも読み取り専用: 検査するデバイス (usbmem_device) には絶対に書かない (読み取り専用で開いたものを渡してよい)。
 * 復元・切り出しは「別の保存先」(ファイル記述子 / パス / ディレクトリ) へ書き出す。保存先が検査するデバイスと同じ媒体の上にならないよう、呼び出し側で選ぶこと。 */
#ifndef USBMEM_C_API_UNDELETE_H
#define USBMEM_C_API_UNDELETE_H
#include <stddef.h>

#include "usbmem/c_api.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 進捗とキャンセル */
typedef struct usbmem_ujob usbmem_ujob;
usbmem_ujob* usbmem_ujob_new(void);
void usbmem_ujob_cancel(usbmem_ujob* job);
/* phase: undelete = "alloc" / "scan" / "verify" / "extract"、carve = "alloc" / "scan" / "extract"。done / total = 段階内の進み具合 (total 0 = 不明。
 * carve の scan はバイト数、undelete の scan はディレクトリ数 / inode のグループ数 / MFT レコード数) */
void usbmem_ujob_progress(usbmem_ujob* job, char* phase, size_t phase_len, unsigned long long* done, unsigned long long* total);
void usbmem_ujob_free(usbmem_ujob* job);

/* ---- 削除ファイルの一覧と救出 */
typedef struct usbmem_undelete usbmem_undelete;
/* dev (FS の先頭から始まるデバイス。パーティションなら usbmem_device_sub_range で切り出す) の削除ファイルを列挙する。FAT12/16/32・exFAT・ext2/3/4・NTFS。
 * 未対応の FS は Errc::Unsupported。キャンセルされると Errc::Cancelled を返すが、それまでの結果を持つハンドルを *out に返す。 */
int usbmem_undelete_scan(usbmem_device* dev, int include_empty, usbmem_ujob* job, usbmem_undelete** out);
void usbmem_undelete_close(usbmem_undelete* h);
/* {"fs","allocationKnown","truncated","cancelled","entriesSeen","skippedInvalid","warnings":[..],
 *  "files":[{"id","name","path","size","mtime","namePartial","inDeletedDir","node","blockBytes","totalBlocks","reusedBlocks","allocationKnown","layoutExact",
 *            "overlapsOtherDeleted","resident","signature","signatureCheck":"match|mismatch|unknown","confidence":0-100,"level":"high|medium|low|none",
 *            "recoverable","notes":[..]}]} */
int usbmem_undelete_result_json(usbmem_undelete* h, char** out_json);
/* files[id] を out_fd へ書き出す (fd は閉じない。SAF の ParcelFileDescriptor.detachFd の値など)。*out_json: {"ok","code","bytes","sha256"}。
 * 書き出せなかった・取り消されたときも JSON は返る (ok = false)。level "none" のファイルは Errc::Unsupported。 */
int usbmem_undelete_extract_fd(usbmem_undelete* h, unsigned id, int out_fd, usbmem_ujob* job, char** out_json);
/* 同、パスへ (新規作成。失敗・取り消しなら作りかけを消す)。 */
int usbmem_undelete_extract_path(usbmem_undelete* h, unsigned id, const char* path, usbmem_ujob* job, char** out_json);

/* ---- カービング */
typedef struct usbmem_carve usbmem_carve;
typedef struct usbmem_carve_options {
  unsigned long long min_size;   /* これ未満は出さない (0 = 制限なし) */
  unsigned long long max_size;   /* 0 = 既定 (16 GiB) */
  unsigned align;                /* ファイルの先頭の置き場所の単位。0 = 512。2 のべき乗 */
  int unallocated_only;          /* FS の割り当て済みの領域を飛ばす (判別できなければ全体) */
  unsigned long long max_hits;   /* 0 = 既定 (1000000) */
} usbmem_carve_options;
/* 形式の一覧: [{"id","extensions","description"}] */
int usbmem_carve_types_json(char** out_json);
/* dev を走査する。types_csv: "jpeg,png" のように形式の id を ',' で区切る (NULL / "" = すべて)。
 * state_path: 進捗 (再開用。mapfile 風のテキスト) を保存するファイル。既にあれば読んで続きから行う (別のデバイス・別の設定のものなら InvalidArgument、壊れていれば Corrupt)。
 *   NULL なら保存しない。保存は一時ファイル + fsync + rename で原子的。
 * out_dir: NULL でなければ、見つけたファイルをこのディレクトリへ見つけ次第書き出す (名前は結果の hits[].name)。NULL なら走査だけで、後から usbmem_carve_extract_* で取り出す。
 * キャンセルされると Errc::Cancelled を返すが、それまでの結果を持つハンドルを *out に返す (state_path から再開できる)。 */
int usbmem_carve_run(usbmem_device* dev, const char* types_csv, const usbmem_carve_options* opt, const char* state_path, const char* out_dir, usbmem_ujob* job,
                     usbmem_carve** out);
void usbmem_carve_close(usbmem_carve* h);
/* {"fs","allocationKnown","cancelled","finished","pos","devSize","bytesScanned","readErrors","rejected","warnings":[..],
 *  "hits":[{"offset","length","format","ext","detail","confidence","name"}]} */
int usbmem_carve_result_json(usbmem_carve* h, char** out_json);
/* hits[index] を書き出す。*out_json: {"ok","code","bytes","sha256"} */
int usbmem_carve_extract_fd(usbmem_carve* h, unsigned index, int out_fd, usbmem_ujob* job, char** out_json);
int usbmem_carve_extract_path(usbmem_carve* h, unsigned index, const char* path, usbmem_ujob* job, char** out_json);

#ifdef __cplusplus
}
#endif
#endif
