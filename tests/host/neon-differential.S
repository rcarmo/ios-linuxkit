#include "asbestos/guest-arm64/gadgets-aarch64/gadgets.h"
.text
.global NAME(neon_reference)
NAME(neon_reference):
    stp x19, x30, [sp, #-80]!
    stp d8, d9, [sp, #16]
    stp d10, d11, [sp, #32]
    stp d12, d13, [sp, #48]
    stp d14, d15, [sp, #64]
    mov x19, x1
    .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    ldr q\n, [x19, #(\n * 16)]
    .endr
    blr x0
    .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21,22,23,24,25,26,27,28,29,30,31
    str q\n, [x19, #(\n * 16)]
    .endr
    ldp d8, d9, [sp, #16]
    ldp d10, d11, [sp, #32]
    ldp d12, d13, [sp, #48]
    ldp d14, d15, [sp, #64]
    ldp x19, x30, [sp], #80
    ret
