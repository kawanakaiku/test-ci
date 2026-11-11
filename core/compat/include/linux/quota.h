/* SPDX-License-Identifier: GPL-2.0-or-later */
/* ext4 移植用シム。ホストでは空 (必要な定義は compat/more_ext4.h)。Android では bionic の <fcntl.h> 等が同名の uapi ヘッダを内部で取り込むため、隠さずに次の検索パスへ回す。 */
#pragma once
#include <compat/kernel.h>
#ifdef __ANDROID__
#include_next <linux/quota.h>
#endif
