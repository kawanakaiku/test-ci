#ifdef __x86_64__
#define SYMBOL(name) name
#else
#define SYMBOL(name) _##name
#endif

    .text

    .macro divide name, op, bits, extend=0
    .globl \name, \name\()_fault, \name\()_end
\name:
#ifdef __x86_64__
    movq %rcx, %rax
#else
    movl 4(%esp), %eax
    movl 12(%esp), %edx
    movl 20(%esp), %ecx
#endif
    .if \extend
        .if \bits == 32
            cltd
        .else
            cqto
        .endif
    .endif
\name\()_fault:
    .if \bits == 32
#ifdef __x86_64__
        \op\()l %r8d
#else
        \op\()l %ecx
#endif
    .else
        \op\()q %r8
    .endif
\name\()_end:
    ret
    .endm

    divide SYMBOL(signed32), idiv, 32
    divide SYMBOL(unsigned32), div, 32
    divide SYMBOL(extended32), idiv, 32, 1

#ifdef __x86_64__
    divide SYMBOL(signed64), idiv, 64
    divide SYMBOL(unsigned64), div, 64
    divide SYMBOL(extended64), idiv, 64, 1
#endif
