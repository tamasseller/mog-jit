/* linker.ld's rom region, grown for the two instrumented images.
 *
 * Same microbit model and the same 8KB ram, but neither assert strings nor
 * a trace-pc call per basic block fit the shipped image's 16KB rom: the
 * assert build is about 21KB and the coverage build about 16KB. rom's
 * ORIGIN+LENGTH is again exactly the batch's own start, so the linker still
 * guarantees image and batch never overlap; that address is
 * -DPPL_BATCH_ADDR in the Makefile and the two must move together. 32KB
 * here leaves the 128KB batch window above it inside the model's 256KB
 * flash.
 */

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

    .init_array : {
        __init_array_start = .;
        KEEP(*(SORT(.init_array.*)))
        KEEP(*(.init_array*))
        __init_array_end = .;
    } > rom

    .data : { *(.data*) } > rom

    .bss : {
        __bss_start = .;
        *(.bss*)
        __bss_end = .;
    } > ram
}
