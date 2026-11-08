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

// Observe HCI Disconnect commands without changing arguments or return values.
.global bridge_disconnect_send
bridge_disconnect_send:
    stp x8, x9, [sp, #-16]!
    adr x8, bridge_state
    ldr x8, [x8]
    cbz x8, 2f
    str x30, [x8, #472]
    mov w9, w0
    bfi w9, w1, #16, #8
    str w9, [x8, #460]
    ldr w9, [x8, #456]
    add w9, w9, #1
    str w9, [x8, #456]
2:
    ldp x8, x9, [sp], #16
    stp x29, x30, [sp, #-0x20]!
    b fw_disconnect_send_continue

// HCI Disconnect Complete parameters: status, handle LE16, reason.
.global bridge_disconnect_receive
bridge_disconnect_receive:
    stp x8, x9, [sp, #-16]!
    adr x8, bridge_state
    ldr x8, [x8]
    cbz x8, 3f
    ldr w9, [x0]
    str w9, [x8, #468]
    ldr w9, [x8, #464]
    add w9, w9, #1
    str w9, [x8, #464]
3:
    ldp x8, x9, [sp], #16
    stp x29, x30, [sp, #-0x20]!
    b fw_disconnect_receive_continue
