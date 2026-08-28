/* Adapted from test/qemu/linker.ld — same lm3s811evb model, same 8KB ram
 * (which that suite's own Makefile already passes as `-m 8k`), but a
 * deliberately *smaller* rom region than its 32KB.
 *
 * The reason is exec_runner.cpp's batch: it arrives via `-device
 * loader,addr=BATCH_ADDR` straight into flash above the image, so the rom
 * region's own end is what fixes how large a batch can be. Measured, not
 * assumed: this model's flash ends at 0xA000, so a 32KB rom region left
 * only 8KB of batch window. 16KB here (against an image of about 10KB)
 * gives a 24KB window instead, and — the part that matters — makes the
 * *linker* the thing that guarantees image and batch never overlap: grow
 * the image past this region and the link fails outright rather than
 * quietly running over the batch at BATCH_ADDR. Keep the two constants in
 * step: rom's ORIGIN+LENGTH here is exec_runner.cpp's BATCH_ADDR.
 *
 * _stack_top is what vectors.S loads as the initial sp, derived from the
 * ram region's own ORIGIN+LENGTH here rather than hardcoded — see
 * test/qemu/linker.ld's header for the history behind that.
 */

MEMORY
{
    rom : ORIGIN = 0x00000000, LENGTH = 0x4000
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
