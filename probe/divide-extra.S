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
    .if \bits == 8
#ifdef __x86_64__
        \op\()b %r8b
#else
        \op\()b %cl
#endif
    .elseif \bits == 16
#ifdef __x86_64__
        \op\()w %r8w
#else
        \op\()w %cx
#endif
    .elseif \bits == 32
#ifdef __x86_64__
        \op\()l %r8d
#else
        \op\()l %ecx
#endif
    .else
        \op\()q %r8
    .endif
\name\()_end:
    .if \bits == 8
        movzbl %al, %eax
    .elseif \bits == 16
        movzwl %ax, %eax
    .endif
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

    divide SYMBOL(signed8), idiv, 8
    divide SYMBOL(unsigned8), div, 8
    divide SYMBOL(signed16), idiv, 16
    divide SYMBOL(unsigned16), div, 16
