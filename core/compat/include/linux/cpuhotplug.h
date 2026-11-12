/* SPDX-License-Identifier: GPL-2.0-or-later */
/* erofs が include するだけ (CONFIG_EROFS_FS_PCPU_KTHREAD は無効なので中身は使われない)。 */
#ifndef USBMEM_LINUX_CPUHOTPLUG_H
#define USBMEM_LINUX_CPUHOTPLUG_H
enum cpuhp_state { CPUHP_AP_ONLINE_DYN = 1 };
#endif
