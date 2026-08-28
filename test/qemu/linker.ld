/* Local copy of jit-armv6m/src/linker.ld with larger rom *and* ram regions
 * — a real translator plus the real dispatch/eviction runtime, linked
 * together, no longer fits the shared script's 4KB rom (sized for the old
 * memcpy-only mock), and grew further once this test's own fixture corpus
 * did (LOOP/BR_TABLE/comparisons/unary ops/eviction/enter_program_on_stack
 * /_split coverage — a much larger .rodata than the original 7
 * straight-line-only fixtures), and again when the TRAP-unwinding
 * fixtures (38-40) landed. 40KB is what this QEMU machine model actually
 * decodes as flash — measured, not the 64KB the datasheet implies: a
 * `-device loader` write past 0xA000 is silently dropped, which is how
 * fuzz/qemu_exec found the real ceiling.
 *
 * ram was bumped for a real reason, not just symmetry with rom: the real
 * translator's own recursive translateBody (one C call frame per open
 * LOOP/BR_TABLE nesting level, decode_instr.h/binops.h/blocks.h's own
 * call chains underneath it) needs noticeably more C stack than the old
 * mock translator's shallow, non-recursive compileProc ever did, and a
 * run covering all 18 fixtures genuinely walked the live stack pointer
 * down far enough to collide with .bss (confirmed via `qemu-system-arm
 * -d exec`: execution ended up jumping into RAM addresses inside
 * fixtures.cpp's own g_scratch, then straight into the fault vector's
 * own `hang` spin-loop). Root cause was two-fold: .bss growing with this
 * test's own larger fixture corpus left less headroom below the stack
 * to begin with, *and* — the part a bigger ram region alone doesn't fix
 * — src/vectors.S's own initial sp is `_stack_top`, a linker symbol this
 * script (like the shared one) now defines as ram's own ORIGIN+LENGTH;
 * bumping ram here without that symbol tracking it would have left the
 * stack anchored at the *old*, smaller ram's own top regardless, since
 * vectors.S never re-derives it from LENGTH(ram) on its own. The
 * lm3s811evb model actually has 8KB of SRAM — this test's own Makefile
 * already passes `-m 8k` to QEMU for exactly that reason — so raising
 * ram to match is the real target's own actual capacity, not a
 * workaround. Keep in sync with jit-armv6m/src/linker.ld's own
 * .init_array/.data/.bss/_stack_top handling if that one ever changes. */

MEMORY
{
    rom : ORIGIN = 0x00000000, LENGTH = 0xA000
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
