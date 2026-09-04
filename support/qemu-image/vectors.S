// Minimal Cortex-M0 vector table + reset handler.
//
// Must be the first object file on the link line (see test/qemu/Makefile) —
// nothing else enforces vector-table placement at address 0.

.syntax unified
.cpu cortex-m0
.thumb

.thumb_func
.global _start
_start:
stacktop: .word _stack_top   // linker-provided RAM top; tracks the ram region's actual size
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
.word sysTickHandler

.thumb_func
reset:
    // Zero .bss (symbols from linker.ld) — RAM has no guaranteed initial
    // content on real hardware; QEMU's own fresh RAM happening to already
    // read as zero is not something to rely on.
    ldr  r0, =__bss_start
    ldr  r1, =__bss_end
    movs r2, #0
bssZeroLoop:
    cmp  r0, r1
    bhs  bssZeroDone
    str  r2, [r0]
    adds r0, #4
    b    bssZeroLoop
bssZeroDone:

    // Run global C++ constructors (.init_array, symbols from linker.ld) —
    // required for any self-registering static object (1test's TEST(...))
    // whose constructor body has an observable side effect beyond setting
    // up its own vtable pointer.
    ldr  r4, =__init_array_start
    ldr  r5, =__init_array_end
ctorLoop:
    cmp  r4, r5
    bhs  ctorDone
    ldr  r3, [r4]
    blx  r3
    adds r4, #4
    b    ctorLoop
ctorDone:

    bl main
    b hang

.weak sysTickHandler         // only test_cancel.cpp defines a real one
.thumb_set sysTickHandler, hang

.thumb_func
hang:   b .
