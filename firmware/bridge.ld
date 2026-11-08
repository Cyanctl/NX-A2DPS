ENTRY(bridge_entry)
SECTIONS {
    . = 0x117A70;
    .bridge : {
        KEEP(*(.text.entry))
        *(.text*)
        *(.rodata*)
        . = ALIGN(8);
        KEEP(*(.bridge_pointer))
    }
    /DISCARD/ : { *(.comment) *(.eh_frame*) *(.note*) }
}
fw_alloc = 0x89070;
fw_free = 0x892F0;
fw_copy = 0x114170;
fw_zero = 0x1142F0;
fw_original_continue = 0x116C4;
fw_config_response = 0x8F740;
fw_disconnect_send_continue = 0xD17B4;
fw_disconnect_receive_continue = 0xBD9D4;
ASSERT(SIZEOF(.bridge) <= 0x590, "Bridge exceeds verified unused executable space")
