/* Minimal linker script for the QEMU lm3s811evb smoke/test image.
 * Adapted from tamasseller/sdvm, jit-prototyping branch, jit/memmap —
 * .init_array/.data/.bss symbol exports added: vectors.S's reset handler
 * needs __init_array_start/__init_array_end to run global C++ constructors
 * and __bss_start/__bss_end to zero .bss, neither of which the sdvm
 * original needed (see vectors.S's own comment). */

MEMORY
{
    rom : ORIGIN = 0x00000000, LENGTH = 0x1000
    ram : ORIGIN = 0x20000000, LENGTH = 0x1000
}

/* vectors.S's own initial sp — the top of ram, by definition (nothing
 * else lives above the stack in this memory map). Kept as a linker
 * symbol rather than a literal baked into vectors.S itself so a build
 * with a differently-sized ram region (any linker script bumping
 * LENGTH above) gets the matching stack top automatically, instead of
 * a stale boundary that silently doesn't grow with it. */
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

    /* Placed directly in rom, not copied to ram at startup: this image
     * never writes to a static-storage-duration object (only ever reads
     * one, e.g. a vtable pointer baked in as constant initialization) —
     * a real hardware target with actually-read-only flash would need a
     * load-address/run-address split and a startup copy loop instead. */
    .data : { *(.data*) } > rom

    .bss : {
        __bss_start = .;
        *(.bss*)
        __bss_end = .;
    } > ram
}
