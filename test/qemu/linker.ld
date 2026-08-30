/* Local copy of jit-armv6m/src/linker.ld with larger rom *and* ram regions:
 * a real translator plus the real dispatch/eviction runtime, linked together
 * with every TEST in this directory, does not fit the shared script's 4KB rom
 * (sized for the old memcpy-only mock). 256KB is the microbit model's own
 * nRF51 flash; fuzz/qemu_exec's 40KB ceiling is a property of `-device
 * loader`, not of the machine.
 *
 * ram stays at 8KB though the model has 16KB: _stack_top sets the whole
 * excursion budget, so test_stack_budget.cpp's numbers move with it.
 * src/vectors.S takes its initial sp from _stack_top, which is defined here
 * rather than re-derived from LENGTH(ram) there — so the two have to move
 * together. Keep in sync with jit-armv6m/src/linker.ld's own
 * .init_array/.data/.bss/_stack_top handling if that one ever changes. */

MEMORY
{
    rom : ORIGIN = 0x00000000, LENGTH = 0x40000
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
