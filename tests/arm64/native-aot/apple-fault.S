#include "cpu-offsets.h"
.text
.p2align 2
.globl _apple_probe_call
_apple_probe_call:
    stp x19, x20, [sp, #-112]!
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    mov x8, sp
    str x8, [x1, #LOCAL_jit_exit_sp]
    mov x19, #0x1357
    movi v0.16b, #0x37
    cmp xzr, xzr
    blr x0
    mov w0, #0
    b _apple_probe_return
.globl _jit_crash_trampoline
_jit_crash_trampoline:
    str x19, [x1, #CPU_x19]
    str q0, [x1, #CPU_fp]
    mrs x8, nzcv
    str w8, [x1, #CPU_nzcv]
    mov w0, #0x100
_apple_probe_return:
    ldp x21, x22, [sp, #16]
    ldp x23, x24, [sp, #32]
    ldp x25, x26, [sp, #48]
    ldp x27, x28, [sp, #64]
    ldp x29, x30, [sp, #80]
    ldp x19, x20, [sp], #112
    ret
.globl _apple_probe_text_start
.globl _apple_probe_read
_apple_probe_text_start:
_apple_probe_read:
    ldr x3, [x2]
    ret
.globl _apple_probe_write
_apple_probe_write:
    str x3, [x2]
    ret
.globl _apple_probe_text_end
_apple_probe_text_end:
