/* r5q twin: one image, loaded by QEMU at the top 64 MiB of a 3 GiB guest (never reached by the code under test). */
ENTRY(_start)
SECTIONS
{
  . = 0xFC000000;
  .text : { *(.text.boot) *(.text*) }
  .rodata : { *(.rodata*) }
  .data : { *(.data*) }
  . = ALIGN(4096);
  .bss : { *(.bss*) *(COMMON) }
  . = ALIGN(16);
  . = . + 0x40000;  el1_stack_top = .;
  . = . + 0x40000;  el2_stack_top = .;
}
