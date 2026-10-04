#include "asbestos/guest-arm64/gadgets-aarch64/gadgets.h"
.text
.global NAME(integer_reference)
NAME(integer_reference):
    stp x19, x30, [sp, #-32]!
    str x0, [sp, #16]
    mov x19, x1
    ldr w8, [x19, x2]
    msr nzcv, x8
    ldp x0, x1, [x19, #16]
    ldp x2, x3, [x19, #32]
    ldp x4, x5, [x19, #48]
    ldp x6, x7, [x19, #64]
    ldr x8, [sp, #16]
    blr x8
    stp x0, x1, [x19, #16]
    stp x2, x3, [x19, #32]
    stp x4, x5, [x19, #48]
    stp x6, x7, [x19, #64]
    mrs x0, nzcv
    ldp x19, x30, [sp], #32
    ret
