/* SPDX-License-Identifier: GPL-2.0 */
/* PSI (メモリストール計測) は使わない */
#pragma once
#include <compat/kernel.h>
static inline void psi_memstall_enter(unsigned long *flags) { *flags = 0; }
static inline void psi_memstall_leave(unsigned long *flags) { (void)flags; }
