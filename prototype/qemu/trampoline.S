// Translator-entry trampoline (docs/jit-armv6m-dispatch-handoff.html §06) —
// what every uncompiled dispatch slot's `code_ptr` points at. Reached by a
// bare `BX` from `callHelper`/`returnHelper`/the prologue stub, exactly like
// any other slot target: r0 = acc (must survive), r1 = slotAddr, r2 =
// offset+1 (must survive).
//
// Unlike callHelper/returnHelper/the prologue stub (src/runtime.ts — plain
// bytes, no absolute addresses, safe to TS-emit and memcpy anywhere), this
// one has to make a real call into the mock translator's C body
// (`compileProc`), whose address isn't known until link time — so it's
// hand-written and built by the normal toolchain instead, same as
// ../../src/vectors.S's own reset handler.
//
// r0/r1/r2 are a mirrored round-trip across the call (window.ts's own
// "same registers in, same registers out" case) — no per-register
// reasoning needed. r8/r9/r10/r11 need no saving here: they're AAPCS
// callee-saved, and compileProc is built with -ffixed-r8/r9/r10/r11
// (qemu/Makefile) so the compiler can't touch them at all.

.syntax unified
.cpu cortex-m0
.thumb

.thumb_func
.global translator_trampoline
translator_trampoline:
    push {r0, r1, r2}
    mov  r3, r8              @ low-mirror the dispatch table base
    subs r0, r1, r3           @ r0 = slotAddr - dispatchBase
    lsrs r0, r0, #3            @ r0 = idx (the *only* argument compileProc needs)
    bl   compileProc
    pop  {r0, r1, r2}          @ restore acc, slotAddr, offset+1
    ldr  r3, [r1, #0]          @ r3 = the code_ptr compileProc just wrote
    bx   r3
