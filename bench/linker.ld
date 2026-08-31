/* Adapted from test/qemu/linker.ld — same microbit model, same 256KB rom,
 * but the full 16KB ram the nRF51 model actually has rather than that
 * script's deliberate 8KB.
 *
 * test/qemu caps ram at 8KB because test_stack_budget.cpp's numbers are
 * stated against _stack_top and have to stay put. Nothing here asserts on a
 * stack figure, and this image has to hold the 2KB output ring and the
 * code arena beside the translator's own recursion, so it takes the model
 * at its word.
 *
 * No `-device loader` window to leave clear either: the program and its
 * input samples are generated into .rodata, so rom is simply the model's
 * whole flash.
 *
 * _stack_top is what vectors.S loads as the initial sp, derived from the
 * ram region's own ORIGIN+LENGTH here rather than hardcoded — see
 * test/qemu/linker.ld's header for the history behind that.
 */

MEMORY
{
    rom : ORIGIN = 0x00000000, LENGTH = 0x40000
    ram : ORIGIN = 0x20000000, LENGTH = 0x4000
}

_stack_top = ORIGIN(ram) + LENGTH(ram);

SECTIONS
{
    .text : { *(.text*) } > rom

    /* The input samples are pinned, not merely placed.
     *
     * Every SAMPLE_AT the translator emits materializes this buffer's
     * address, and Assembler::materializeImm32 chooses between a pooled
     * literal and a two-instruction immediate sequence *by the value* — so
     * where the array lands changes how many instructions the inner loop
     * costs. Left in ordinary .rodata it moves with .text, and .text moves
     * with the reference kernels' optimization level, which made the JIT's
     * own per-sample figure differ by a whole instruction between images
     * that were supposed to differ only in the C side. Measured, not
     * theorised: 19.05 against 18.05 instructions per sample at -O0 versus
     * -O1.
     *
     * Pinning it well clear of the image (~21KB) removes the confound and
     * costs nothing — the driver asserts the JIT-side numbers now match
     * across all six images, which is what makes them reproducible. */
    .samples 0x00030000 : { KEEP(*(.samples)) } > rom

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
