#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <windows.h>

typedef uintptr_t (*divide_fn)(uint64_t, uint64_t, uint64_t);

#define DECLARE(name) \
    extern uintptr_t name(uint64_t, uint64_t, uint64_t); \
    extern const char name##_fault, name##_end

DECLARE(signed32);
DECLARE(unsigned32);
DECLARE(extended32);
#ifdef _WIN64
DECLARE(signed64);
DECLARE(unsigned64);
DECLARE(extended64);
#endif

struct division {
    const char *name;
    divide_fn divide;
    const char *fault;
    const char *end;
    uint64_t lower, upper, divisor;
};

#define TEST(label, fn, lo, hi, by) {label, fn, &fn##_fault, &fn##_end, lo, hi, by}

static const struct division cases[] = {
    TEST("idiv32/normal", extended32, 13, 0, 3),
    TEST("idiv32/zero", extended32, 13, 0, 0),
    TEST("idiv32/min/-1", extended32, 0x80000000, 0, UINT32_MAX),
    TEST("idiv32/wide/overflow", signed32, 0, 1, 1),
    TEST("idiv32/wide/zero", signed32, 0, 1, 0),
    TEST("div32/normal", unsigned32, 13, 0, 3),
    TEST("div32/zero", unsigned32, 13, 0, 0),
    TEST("div32/overflow", unsigned32, 0, 1, 1),
    TEST("div32/wide/normal", unsigned32, 0, 1, 2),
#ifdef _WIN64
    TEST("idiv64/normal", extended64, 13, 0, 3),
    TEST("idiv64/zero", extended64, 13, 0, 0),
    TEST("idiv64/min/-1", extended64, UINT64_C(0x8000000000000000), 0, UINT64_MAX),
    TEST("idiv64/wide/overflow", signed64, 0, 1, 2),
    TEST("idiv64/wide/zero", signed64, 0, 1, 0),
    TEST("idiv64/wide/normal", signed64, 0, 1, 4),
    TEST("idiv64/wide/negative", signed64, 0, UINT64_MAX - 1, 8),
    TEST("div64/normal", unsigned64, 13, 0, 3),
    TEST("div64/zero", unsigned64, 13, 0, 0),
    TEST("div64/overflow", unsigned64, 0, 1, 1),
    TEST("div64/wide/zero", unsigned64, 0, 1, 0),
    TEST("div64/wide/normal", unsigned64, 0, 1, 2),
#endif
};

struct observation {
    DWORD code;
    uintptr_t address, ip;
    uintptr_t quotient;
};

static LONG capture(struct observation *result, const EXCEPTION_RECORD *record, uintptr_t ip)
{
    result->code = record->ExceptionCode;
    result->address = (uintptr_t)record->ExceptionAddress;
    result->ip = ip;
    return EXCEPTION_EXECUTE_HANDLER;
}

static void run_case(const struct division *test, struct observation *result)
{
#ifdef _WIN64
    __try {
        result->quotient = test->divide(test->lower, test->upper, test->divisor);
    } __except (capture(result, GetExceptionInformation()->ExceptionRecord,
                        GetExceptionInformation()->ContextRecord->Rip)) {
    }
#else
    /* clang/MinGW の i686 では __try のテーブルを生成できないため、
       下の x86 用ハンドラーを SEH チェーンに登録する。 */
    extern void run_x86_seh(const struct division *, struct observation *);
    run_x86_seh(test, result);
#endif
}

#ifndef _WIN64
struct seh_frame {
    EXCEPTION_REGISTRATION_RECORD record;
    const struct division *test;
    struct observation *result;
};

static EXCEPTION_DISPOSITION NTAPI handle_x86(EXCEPTION_RECORD *exception, void *registration,
                                                CONTEXT *context, void *dispatcher)
{
    struct seh_frame *frame = registration;
    (void)dispatcher;
    if (exception->ExceptionFlags & (EXCEPTION_UNWINDING | EXCEPTION_EXIT_UNWIND))
        return ExceptionContinueSearch;
    if (context->Eip < (uintptr_t)frame->test->fault || context->Eip >= (uintptr_t)frame->test->end)
        return ExceptionContinueSearch;
    capture(frame->result, exception, context->Eip);
    /* divide.S の除算の次は ret。登録側のフレームへそのまま戻る。 */
    context->Eip = (uintptr_t)frame->test->end;
    return ExceptionContinueExecution;
}

void run_x86_seh(const struct division *test, struct observation *result)
{
    struct seh_frame frame = {.record.Handler = handle_x86, .test = test, .result = result};

    __asm__ volatile("movl %%fs:0, %0; movl %1, %%fs:0"
                     : "=&r"(frame.record.Next) : "r"(&frame) : "memory");
    result->quotient = test->divide(test->lower, test->upper, test->divisor);
    __asm__ volatile("movl %0, %%fs:0" : : "r"(frame.record.Next) : "memory");
}
#endif

int main(int argc, char **argv)
{
    FILE *out = argc > 1 ? fopen(argv[1], "w") : stdout;
    if (!out) return 2;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const struct division *test = &cases[i];
        const uintptr_t expected = (uintptr_t)test->fault;
        struct observation result = {0};
        fprintf(out, "%s ", test->name);
        fflush(out);
        run_case(test, &result);
        if (result.code) {
            fprintf(out, "code=%08lx address=%" PRIxPTR " ip=%" PRIxPTR
                    " expected=%" PRIxPTR " at_div=%d\n",
                    (unsigned long)result.code, result.address, result.ip, expected,
                    result.address == expected && result.ip == expected);
        } else {
            fprintf(out, "quotient=%" PRIxPTR "\n", result.quotient);
        }
        fflush(out);
    }
    fputs("done\n", out);
    return out == stdout ? 0 : fclose(out) != 0;
}
