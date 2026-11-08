.section .text.entry,"ax",%progbits
.global bridge_entry
.type bridge_entry,%function
bridge_entry:
    ldr x9, [x2, #8]
    sub x9, x9, #0x3c0
    cmp x9, #3
    b.hi 1f
    ldp x0, x2, [x2]
    b bridge_dispatch
1:
    // Execute the displaced entry instruction before returning to Horizon.
    stp x29, x30, [sp, #-0x10]!
    b fw_original_continue
.size bridge_entry,.-bridge_entry
