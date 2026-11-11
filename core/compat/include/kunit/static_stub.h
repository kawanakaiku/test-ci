/* SPDX-License-Identifier: GPL-2.0-or-later */
/* KUnit は使わない: static stub のリダイレクトは何もしない */
#pragma once
#define KUNIT_STATIC_STUB_REDIRECT(fn, args...) do { } while (0)
#define VISIBLE_IF_KUNIT static
#define EXPORT_SYMBOL_IF_KUNIT(x)
