#pragma once
#include <compat/kernel.h>
/* トレースポイントは使わない: TRACE_EVENT などは「何もしない trace_<name>() 関数」にする (引数の評価もしない)。 */
#define TP_PROTO(args...) args
#define TP_ARGS(args...) args
#define TP_STRUCT__entry(args...)
#define TP_fast_assign(args...)
#define TP_printk(args...)
#define DECLARE_EVENT_CLASS(name, proto, args, tstruct, assign, print)
#define DEFINE_EVENT(cls, name, proto, args) static inline void trace_##name(proto) { }
#define TRACE_EVENT(name, proto, args, tstruct, assign, print) static inline void trace_##name(proto) { }
#define CREATE_TRACE_POINTS
