/* The smallest thing that boots: this image is a probe, not a runner, so it
 * needs no batch window above its rom. */

ENTRY(_start)

MEMORY
{
    rom : ORIGIN = 0x00000000, LENGTH = 0x8000
    ram : ORIGIN = 0x20000000, LENGTH = 0x2000
}

_stack_top = ORIGIN(ram) + LENGTH(ram);

SECTIONS
{
    .text : { *(.text*) } > rom
    .rodata : { *(.rodata*) } > rom
    .init_array : { __init_array_start = .; KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array*)) __init_array_end = .; } > rom
    .data : { *(.data*) } > rom
    .bss : { __bss_start = .; *(.bss*) __bss_end = .; } > ram
}
