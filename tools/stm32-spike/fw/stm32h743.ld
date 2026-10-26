/* Phase 0 spike: STM32H743-shaped memory map (2 MB flash, 512 KB AXI SRAM).
 * Stack lives in DTCM so its size is visible separately from heap/data. */
ENTRY(Reset_Handler)

MEMORY
{
  FLASH (rx)  : ORIGIN = 0x08000000, LENGTH = 2048K
  DTCM  (rw)  : ORIGIN = 0x20000000, LENGTH = 128K
  AXI   (rw)  : ORIGIN = 0x24000000, LENGTH = 512K
}

_estack = ORIGIN(DTCM) + LENGTH(DTCM);

SECTIONS
{
  .isr_vector : { KEEP(*(.isr_vector)) } > FLASH
  .text : { *(.text*) *(.rodata*) KEEP(*(.init)) KEEP(*(.fini)) } > FLASH
  .ARM.exidx : { *(.ARM.exidx*) } > FLASH
  .preinit_array : { PROVIDE_HIDDEN(__preinit_array_start = .); KEEP(*(.preinit_array*)) PROVIDE_HIDDEN(__preinit_array_end = .); } > FLASH
  .init_array : { PROVIDE_HIDDEN(__init_array_start = .); KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array*)) PROVIDE_HIDDEN(__init_array_end = .); } > FLASH
  .fini_array : { PROVIDE_HIDDEN(__fini_array_start = .); KEEP(*(.fini_array*)) PROVIDE_HIDDEN(__fini_array_end = .); } > FLASH

  .tdata : { _stdata = .; *(.tdata*) _etdata = .; } > FLASH
  .tbss : { _stbss = .; *(.tbss*) _etbss = .; } > FLASH

  _sidata = LOADADDR(.data);
  .data : { _sdata = .; *(.data*) _edata = .; } > AXI AT > FLASH
  .bss (NOLOAD) : { _sbss = .; *(.bss*) *(COMMON) _ebss = .; } > AXI
  .heap (NOLOAD) : { _heap_start = .; . = ORIGIN(AXI) + LENGTH(AXI); _heap_end = .; } > AXI
}
