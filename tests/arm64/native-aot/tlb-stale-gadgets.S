// Exercise only the actual gadget inline checks; miss exits return a zero marker.
#include "asbestos/guest-arm64/gadgets-aarch64/gadgets.h"
.text
.global tlb_stale_gadget_read
.type tlb_stale_gadget_read,%function
tlb_stale_gadget_read:
    ldr x7, [x1, #CPU_pc]
    read_prep 64, stale_read
    str x7, [x1, #CPU_pc]
    ret
handle_miss_stale_read:
crosspage_load_stale_read:
    str xzr, [x1, #CPU_pc]
    ret
.global tlb_stale_gadget_write
.type tlb_stale_gadget_write,%function
tlb_stale_gadget_write:
    ldr x7, [x1, #CPU_pc]
    write_prep 64, stale_write
    str x7, [x1, #CPU_pc]
    ret
handle_miss_stale_write:
crosspage_write_prep_stale_write:
    str xzr, [x1, #CPU_pc]
    ret
.section .note.GNU-stack,"",%progbits
