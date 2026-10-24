/* Three page-aligned segments at their NSO offsets: text (RX), rodata (R), data + bss (RW). */
PHDRS { text PT_LOAD FLAGS(5); rodata PT_LOAD FLAGS(4); data PT_LOAD FLAGS(6); dyn PT_DYNAMIC; }
SECTIONS {
  . = 0;
  .text : { KEEP(*(.text.crt0)) *(.text .text.*) } :text
  .plt : { *(.plt) } :text
  . = ALIGN(0x1000);
  .rodata : { KEEP(*(.rodata.mod0)) *(.rodata .rodata.*) } :rodata
  .dynsym : { *(.dynsym) } :rodata
  .dynstr : { *(.dynstr) } :rodata
  .hash : { *(.hash) } :rodata
  .gnu.hash : { *(.gnu.hash) } :rodata
  .rela.dyn : { *(.rela.dyn) } :rodata
  .rela.plt : { *(.rela.plt) } :rodata
  .relr.dyn : { *(.relr.dyn) } :rodata
  .eh_frame_hdr : { __eh_frame_hdr_start = .; *(.eh_frame_hdr) __eh_frame_hdr_end = .; } :rodata
  .eh_frame : { KEEP(*(.eh_frame)) } :rodata
  . = ALIGN(0x1000);
  .dynamic : { *(.dynamic) } :data :dyn
  .got : { *(.got) } :data
  .got.plt : { *(.got.plt) } :data
  .init_array : { KEEP(*(.init_array .init_array.*)) } :data
  .data : { *(.data .data.*) } :data
  __bss_start__ = .;
  .bss : { *(.bss .bss.*) *(COMMON) } :data
  __bss_end__ = .;
  /DISCARD/ : { *(.comment) *(.note*) *(.interp) }
}
