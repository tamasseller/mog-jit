/* Local copy of jit-armv6m/src/linker.ld with a larger rom region — a
 * real translator plus the real dispatch/eviction runtime, linked
 * together, no longer fits the shared script's 4KB (sized for the old
 * memcpy-only mock). 16KB is still a small fraction of the lm3s811evb's
 * real 64KB flash; ram is unchanged (this test only ever calls the plain
 * enter_program variant, never *_on_stack/_split, so the small-procCount
 * Runtime storage it needs is nowhere near the original 4KB budget).
 * Keep in sync with jit-armv6m/src/linker.ld's own .init_array/.data/.bss
 * handling if that one ever changes. */

MEMORY
{
    rom : ORIGIN = 0x00000000, LENGTH = 0x4000
    ram : ORIGIN = 0x20000000, LENGTH = 0x1000
}

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
