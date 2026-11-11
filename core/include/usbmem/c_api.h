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
/* *out_json は usbmem_free で解放する(UTF-8, NUL 終端)。 */
int usbmem_list_partitions_json(usbmem_device* dev, char** out_json);
void usbmem_close(usbmem_device* dev);
void usbmem_free(void* p);
const char* usbmem_errc_name(int code);

#ifdef __cplusplus
}
#endif
#endif
