#pragma once
/* Android(bionic) 内部ヘッダが引く uapi ヘッダ: 実物をそのまま通す (compat/kernel.h は引かない) */
#if defined(__ANDROID__) && defined(__has_include_next)
#if __has_include_next(<asm/byteorder.h>)
#include_next <asm/byteorder.h>
#else
#include <compat/kernel.h>
#endif
#else
#include <compat/kernel.h>
#endif
