// Minimal Cortex-M0 vector table + reset handler.
// Adapted from tamasseller/sdvm, jit-prototyping branch, jit/vectors.s —
// `entry` renamed to a plain C `main` so test binaries can use an ordinary
// int main() as their entry point; the MMIO PUT32 helper wasn't needed for
// this project's smoke tests and was dropped. Zeroing .bss and running
// .init_array (global C++ constructors) before main() is new here — the
// sdvm original's own jit.cpp/jumpy.S didn't use any self-registering
// global objects, so it never needed either step; 1test's TEST(...) macro
// registers itself into a linked list from a global constructor
// (Registry::StaticElement's constructor body), which is a genuine runtime
// side effect the compiler cannot fold into static/constant
// initialization the way a trivial vtable-pointer-only object can — skip
// this and every TEST(...) in the image silently never runs.
//
// Must be the first object file on the link line (see test/qemu/Makefile) —
// nothing here enforces vector-table placement at address 0 beyond that
// link order, matching the sdvm original's own approach.

.syntax unified
.cpu cortex-m0
.thumb

.thumb_func
.global _start
_start:
stacktop: .word 0x20001000
.word reset
.word hang      // NMI
.word hang      // HardFault
.word hang
.word hang
.word hang
.word hang
.word hang
.word hang
.word hang
.word hang      // SVCall
.word hang
.word hang
.word hang      // PendSV
.word hang      // SysTick

.thumb_func
reset:
    // Zero .bss (symbols from linker.ld) — RAM has no guaranteed initial
    // content on real hardware; QEMU's own fresh RAM happening to already
    // read as zero is not something to rely on.
    ldr  r0, =__bss_start
    ldr  r1, =__bss_end
    movs r2, #0
bss_zero_loop:
    cmp  r0, r1
    bhs  bss_zero_done
    str  r2, [r0]
    adds r0, #4
    b    bss_zero_loop
bss_zero_done:

    // Run global C++ constructors (.init_array, symbols from linker.ld) —
    // required for any self-registering static object (1test's TEST(...))
    // whose constructor body has an observable side effect beyond setting
    // up its own vtable pointer.
    ldr  r4, =__init_array_start
    ldr  r5, =__init_array_end
ctor_loop:
    cmp  r4, r5
    bhs  ctor_done
    ldr  r3, [r4]
    blx  r3
    adds r4, #4
    b    ctor_loop
ctor_done:

    bl main
    b hang

.thumb_func
hang:   b .
