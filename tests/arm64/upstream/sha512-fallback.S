/* Linux host ABI probe and native oracle. Raw encodings allow baseline builds. */
.text
.global sha512_fallback_native
.type sha512_fallback_native, %function
sha512_fallback_native:
    ldr q0, [x1]
    ldr q1, [x2]
    ldr q2, [x3]
    cbz w0, 1f
    cmp w0, #1
    b.eq 2f
    cmp w0, #2
    b.eq 3f
    .inst 0xce628420 // sha512h2 q0, q1, v2.2d
    b 4f
1:  .inst 0xcec08020 // sha512su0 v0.2d, v1.2d
    b 4f
2:  .inst 0xce628820 // sha512su1 v0.2d, v1.2d, v2.2d
    b 4f
3:  .inst 0xce628020 // sha512h q0, q1, v2.2d
4:  str q0, [x1]
    ret

.global sha512_fallback_gadget_run
.type sha512_fallback_gadget_run, %function
sha512_fallback_gadget_run:
    stp x19, x20, [sp, #-160]!
    stp x21, x22, [sp, #16]
    stp x23, x24, [sp, #32]
    stp x25, x26, [sp, #48]
    stp x27, x28, [sp, #64]
    stp x29, x30, [sp, #80]
    str x1, [sp, #96]
    str x0, [sp, #104]
    str x18, [sp, #128]
    mrs x8, fpsr
    str x8, [sp, #136]
    str x2, [sp, #112] // code-stream operand, high bits hold NZCV sentinel
    adr x8, .Ldone
    str x8, [sp, #120] // gret continuation
    add x28, sp, #112
    mov x2, #2
    mov x3, #3
    mov x4, #4
    mov x5, #5
    mov x6, #6
    mov x7, #7
    .irp n,19,20,21,22,23,24,25,26,27,29,30
    mov x\n, #\n
    .endr
    // Exercise every NZCV value and both clear/set cumulative FP flags.
    mov x8, #0
    ldr x9, [sp, #112]
    tbz x9, #8, 5f
    mov x8, #0x9f
    movk x8, #0x800, lsl #16
5:  str x8, [sp, #152]
    msr fpsr, x8
    lsr x8, x9, #32
    str x8, [sp, #144]
    msr nzcv, x8
    br x0
.Ldone:
    mrs x9, nzcv
    mrs x10, fpsr
    ldr x8, [sp, #104]
    cmp x0, x8
    b.ne .Lfailed
    ldr x8, [sp, #128]
    cmp x18, x8
    b.ne .Lfailed
    mov x0, #1
    ldr x8, [sp, #144]
    cmp x9, x8
    b.ne .Lreturn
    ldr x8, [sp, #152]
    cmp x10, x8
    b.ne .Lreturn
    ldr x8, [sp, #96]
    cmp x1, x8
    b.ne .Lreturn
    add x8, sp, #128
    cmp x28, x8
    b.ne .Lreturn
    .irp n,2,3,4,5,6,7,19,20,21,22,23,24,25,26,27,29,30
    cmp x\n, #\n
    b.ne .Lreturn
    .endr
    mov x0, #0
    b .Lreturn
.Lfailed:
    mov x0, #1
.Lreturn:
    ldr x8, [sp, #136]
    msr fpsr, x8
    ldp x29, x30, [sp, #80]
    ldp x27, x28, [sp, #64]
    ldp x25, x26, [sp, #48]
    ldp x23, x24, [sp, #32]
    ldp x21, x22, [sp, #16]
    ldp x19, x20, [sp], #160
    ret
#ifdef SHA512_CLOBBER_HELPERS
// Linker wraps still execute the actual candidate helper, then deliberately
// destroy caller-saved state. This makes the ABI/status probe non-vacuous even
// if the current optimizer happens not to clobber registers or FP status.
.macro clobber_helper name
.global __wrap_\name
.type __wrap_\name, %function
__wrap_\name:
    str x30, [sp, #-16]!
    bl __real_\name
    msr nzcv, xzr
    msr fpsr, xzr
    .irp n,0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18
    mov x\n, #(256 + \n)
    .endr
    ldr x30, [sp], #16
    ret
.endm
clobber_helper sha512su0_helper
clobber_helper sha512su1_helper
clobber_helper sha512h_helper
clobber_helper sha512h2_helper
#endif
.section .note.GNU-stack,"",%progbits
