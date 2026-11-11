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
#define TRACE_DEFINE_ENUM(x)
#define TRACE_DEFINE_SIZEOF(x)
#define TRACE_EVENT_CONDITION(name, proto, args, cond, tstruct, assign, print) static inline void trace_##name(proto) { }
#define DEFINE_EVENT_CONDITION(cls, name, proto, args, cond) static inline void trace_##name(proto) { }
#define DEFINE_EVENT_PRINT(cls, name, proto, args, print) static inline void trace_##name(proto) { }
#define DECLARE_TRACE(name, proto, args)
#define TRACE_EVENT_FN(name, proto, args, tstruct, assign, print, reg, unreg) static inline void trace_##name(proto) { }
#define __field(t, n)
#define __array(t, n, l)
#define __string(n, s)
#define __entry_dummy
