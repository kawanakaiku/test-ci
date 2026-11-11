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
/* *out_json は usbmem_free で解放する(UTF-8, NUL 終端)。 */
int usbmem_list_partitions_json(usbmem_device* dev, char** out_json);
void usbmem_close(usbmem_device* dev);
void usbmem_free(void* p);
const char* usbmem_errc_name(int code);

#ifdef __cplusplus
}
#endif
#endif
