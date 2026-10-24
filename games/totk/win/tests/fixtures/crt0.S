/* Module header of a synthetic Horizon module (as libnx's crt0 lays it out): the text segment starts with a branch
   and the offset of MOD0, which points at the dynamic table, the bss and the eh_frame_hdr. */
    .section .text.crt0, "ax"
    .global __module_start
__module_start:
    b       1f
    .word   __nx_mod0 - __module_start
1:  ret

    /* linker-script symbols, local to the module (not preemptible), so the offsets below link as constants */
    .hidden _DYNAMIC, __bss_start__, __bss_end__, __eh_frame_hdr_start, __eh_frame_hdr_end

    .section .rodata.mod0, "a"
    .align  2
__nx_mod0:
    .ascii  "MOD0"
    .word   _DYNAMIC - __nx_mod0
    .word   __bss_start__ - __nx_mod0
    .word   __bss_end__ - __nx_mod0
    .word   __eh_frame_hdr_start - __nx_mod0
    .word   __eh_frame_hdr_end - __nx_mod0
    .word   0
