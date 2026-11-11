/* SPDX-License-Identifier: Apache-2.0 */
/* JNI など C 境界向けの薄い API。例外は外に出さない。戻り値 0 = 成功、それ以外は usbmem::Errc。 */
#ifndef USBMEM_C_API_H
#define USBMEM_C_API_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct usbmem_device usbmem_device;

/* fd は take_ownership != 0 なら usbmem_close で閉じる。read_only != 0 が既定の使い方。 */
int usbmem_open_fd(int fd, int read_only, int take_ownership, usbmem_device** out);
int usbmem_open_path(const char* path, int read_only, usbmem_device** out);
/* 親デバイスの一区間 [offset, offset+length) を別デバイスとして得る (パーティション用)。範囲外は OutOfRange。 */
int usbmem_device_sub_range(usbmem_device* parent, unsigned long long offset, unsigned long long length, int read_only, usbmem_device** out);
/* USB Mass Storage (Bulk-Only)。fd は UsbDeviceConnection.getFileDescriptor() で、所有権は呼び出し側に残る
 * (usbmem_close の後も fd は閉じない。device より長く生かすこと)。claimInterface は呼び出し側で済ませておく。
 * read_only は常に 1 を推奨 (書き込みは検証済みになるまで有効化しない)。 */
int usbmem_open_usb_msc(int fd, int ep_in, int ep_out, int interface_number, int lun, int read_only, usbmem_device** out);
/* 直近の SCSI センス (診断用)。key/asc/ascq。USB デバイスでなければ 0。 */
void usbmem_usb_last_sense(usbmem_device* dev, int* key, int* asc, int* ascq);
/* 送った SCSI オペコードの集計文字列を buf (NUL 終端) へ。USB マスストレージでなければ空。 */
/* 直近の USB マスストレージ open の診断記録 (SCSI コマンドと結果) を buf へ。 */
void usbmem_usb_last_open_trace(char* buf, size_t len);
void usbmem_usb_op_stats(usbmem_device* dev, char* buf, size_t len);
/* 転送サイズの調整 (USB マスストレージのみ。0 = 変更しない)。transport_chunk: 1 回の USB 転送要求 / max_transfer: 1 SCSI コマンドあたり (バイト)。 */
/* ディスクの回転を止める (start=0) / 再開 (start=1)。SCSI START STOP UNIT (メディアの内容は変えない)。0 / <0 = -Errc (未対応は Unsupported) */
int usbmem_usb_start_stop(usbmem_device* dev, int start);
/* アイドル自動スピンダウン: 最後の I/O から idle_ms 以上たっていれば止める。1 = 止めた / 0 = 何もしない / <0 = -Errc。止めた後の I/O は自動で再開してから行う */
int usbmem_usb_idle_stop(usbmem_device* dev, unsigned long long idle_ms);
/* 回転を止めている (START STOP UNIT で止めた) なら 1 */
int usbmem_usb_spun_down(usbmem_device* dev);
int usbmem_usb_tune(usbmem_device* dev, size_t transport_chunk, size_t max_transfer);
/* 読み取り専用の速度計測: offset から total バイトを buf_len ごとに順に読み、経過マイクロ秒を *out_us に返す。 */
/* out_hash (NULL 可): 読んだデータ全体の 64 ビットハッシュ。転送サイズを変えても同じ内容が読めること (化けていないこと) の確認用 */
int usbmem_bench_read(usbmem_device* dev, unsigned long long offset, unsigned long long total, size_t buf_len, long long* out_us, unsigned long long* out_hash);
/* *out_json は usbmem_free で解放する(UTF-8, NUL 終端)。 */
int usbmem_list_partitions_json(usbmem_device* dev, char** out_json);
void usbmem_close(usbmem_device* dev);
void usbmem_free(void* p);
const char* usbmem_errc_name(int code);

#ifdef __cplusplus
}
#endif
#endif
