// The fixed, flash-resident runtime machinery (docs/jit-armv6m-dispatch-
// handoff.html §06): callHelper, returnHelper, the translator-entry
// trampoline, and enter_dispatch. All four sit at one link-time-fixed
// address for the whole program's lifetime, reached via the r10 helper
// vector (the first two), a dispatch slot's initial `code_ptr` (the
// trampoline), or an ordinary `bl` from runtime_host.c (enter_dispatch) —
// never copied anywhere, unlike the per-procedure prologue stub
// (src/runtime.ts, which *is* copied into the arena as the first bytes of
// every compiled procedure, so it has to stay data the mock translator can
// memcpy, not code reached by name). That's the actual dividing line
// between what lives here and what's TS-emitted — not "which one is
// simpler to write," and not "which one needs a real linked `bl`" either,
// even though only the trampoline and enter_dispatch happen to need one:
// callHelper/returnHelper *could* be TS-emitted bytes just as easily (no
// absolute addresses, self-contained), but there's no reason they should
// be, once being fixed and reached by name is what actually distinguishes
// them from the stub. Hand-written and built by the normal toolchain, same
// as ../../src/vectors.S's own reset handler.

.syntax unified
.cpu cortex-m0
.thumb

#include "runtime_host.h" // RUNTIME_DISPATCH_TABLE_OFFSET / DISPATCH_SENTINEL_OFFSET

// in: r1 = packed record (P_idx | K+1<<16), r2 = Q_idx
// out: never returns — tail-jumps into Q's resident code or the translator
//
// The record travels in lr from here on, not on the operand stack: this
// JIT never emits hardware BL/BLX in its own compiled code, so lr is
// otherwise dead, and it's otherwise the same "sp is the real C stack, not
// a second one" reasoning (docs' own §06) applied one step further — no
// separate control-stack region, and now no fixed per-call stack word
// either. A callee that itself makes a nested CALL (translateProc.ts's
// `needsLRSave`) saves this word to its own stack in its own prologue
// instead, once per activation, not here — this routine (and
// returnHelper, below) carries the record's handoff exactly once, in
// flash, never duplicated per call/return site. Nothing here or in
// returnHelper/the prologue stub ever hands off to independently-compiled
// code, so nothing here needs 8-byte alignment at all — the one place
// that genuinely does (translator_trampoline's own call into compileProc,
// and eventually any C-implemented extension helper reached through the
// r10 vector) fixes alignment right there instead, via REALIGN_ENTER/
// REALIGN_LEAVE below.
.thumb_func
.global callHelper
callHelper:
    mov   lr, r1               @ persist the record in lr — r1 is free again immediately after
    lsls  r1, r2, #3          @ slotAddr-relative = Q_idx * 8
    add   r1, r1, r8           @ r1 = slotAddr
    ldr   r3, [r1, #0]          @ r3 = code_ptr
    movs  r2, #1                 @ offset+1 = 1, hardwired — a fresh call never resumes mid-procedure
    bx    r3

// Three entry points feeding one shared tail: which one a procedure's own
// RETURN/TRAP dispatches to (translateProc.ts's `abiRealStrategy.
// emitReturn`, via the helper vector) depends on whether that procedure's
// own prologue pushed {lr}, and — if it did — whether it also has any
// original out-of-window arguments (`argCount > WINDOW_SIZE`) sitting
// *below* that pushed word. `returnHelperTail` is reachable both as a
// fallthrough from `returnHelperFromStack` (the common non-leaf case) and
// directly, by name, from that rare third case: `pop{r1}` alone can
// retrieve the record, but reclaiming the out-of-window arguments below it
// needs a procedure-specific byte count no shared, parameterless routine
// can know — so that one case does its own `pop{r1}`/`incrSp` inline
// (translateProc.ts) and jumps here directly, skipping both fetch variants.
//
// in: nothing (returnHelperFromLr) / the caller's own record already
// pushed by this procedure's own prologue (returnHelperFromStack) / r1
// already holding the record (returnHelperTail, reached directly)
// out: never returns — tail-jumps into the caller's resident code or the translator
.thumb_func
.global returnHelperFromLr
returnHelperFromLr:            @ leaf procedures: the record has sat untouched in lr since entry
    mov   r1, lr
    b     returnHelperTail

.thumb_func
.global returnHelperFromStack
returnHelperFromStack:         @ ordinary non-leaf procedures: retrieve what their own prologue pushed
    pop   {r1}                @ r1 = packed record
.thumb_func
.global returnHelperTail
returnHelperTail:
    lsrs  r2, r1, #16           @ r2 = offset+1
    sxth  r1, r1                  @ r1 = caller_idx, sign-extended — SXTH not UXTH (this
                                    @ session's fix): the sentinel proc_idx (0xffff, "index
                                    @ -1") must resolve to r8-8, not r8+524280
    lsls  r1, r1, #3                 @ slotAddr-relative = caller_idx * 8
    add   r1, r1, r8                  @ r1 = slotAddr
    ldr   r3, [r1, #0]                 @ r3 = code_ptr
    bx    r3

// Realign sp down to an 8-byte boundary right before handing off to
// independently-compiled C — the translator (compileProc) today, and
// eventually any C-implemented extension helper reached through the r10
// vector (§06's reserved slots). Nothing in this file's own hand-written
// excursion (callHelper/returnHelper/the prologue stub/this trampoline
// itself) ever needs the AAPCS 8-byte-SP guarantee, since none of it is
// independently compiled — but the callee on the other side of a real
// `BL`/`BLX` might be, and might genuinely use LDRD/STRD (this is built for
// Cortex-M0 today, which has neither, but the same source may target a
// core that does), or simply assume its own locals land 8-aligned the way
// AAPCS promises every caller will leave them.
//
// Branchless, and doesn't care whether sp happens to already be aligned:
// always reserves a full 8 bytes below the true sp (enough room for the
// stash below regardless of whether rounding needed 0 or 4 of it), rounds
// down, then stashes the real sp at a fixed +4 offset into that slack
// rather than pushing it — a push after rounding down would just
// re-misalign sp for the call that follows. Clobbers r2/r3 only.
.macro REALIGN_ENTER
    mov  r3, sp
    lsrs r2, r3, #3
    lsls r2, r2, #3            @ r2 = old sp rounded down to 8 (Rd!=Rn shifts have no 3-bit-imm limit)
    subs r2, r2, #8             @ Rd==Rn here, so the 8-bit-immediate SUBS form applies (unlike Rd!=Rn, which caps at 3 bits)
    mov  sp, r2
    str  r3, [sp, #4]
.endm

.macro REALIGN_LEAVE
    ldr  r3, [sp, #4]
    mov  sp, r3
.endm

// Translator-entry trampoline — what every uncompiled dispatch slot's
// `code_ptr` points at. Reached by a bare `BX` from `callHelper`/
// `returnHelper`/the prologue stub, exactly like any other slot target:
// r0 = acc (must survive), r1 = slotAddr, r2 = offset+1 (must survive).
// The one piece of this file that genuinely *needs* to be hand-written
// rather than TS-emitted bytes: it makes a real call into the mock
// translator's C body (`compileProc`), whose address isn't known until
// link time.
//
// r0/r1/r2 are a mirrored round-trip across the call (window.ts's own
// "same registers in, same registers out" case) — no per-register
// reasoning needed. r8/r9/r10/r11 need no saving here: they're AAPCS
// callee-saved, and compileProc is built with -ffixed-r8/r9/r10/r11
// (qemu/Makefile) so the compiler can't touch them at all. lr is the odd
// one out: it's not an argument that needs mirroring, but it does hold
// whatever call/return record was live when this trampoline was reached
// (callHelper/returnHelper's own contract now, since the record travels
// in lr rather than on the stack — see above), and `bl compileProc`
// clobbers it same as any ordinary call. Saved and restored the long way
// (through r3, POP has no form that targets lr directly) purely because
// this is the one place in this file that makes a real call at all.
.thumb_func
.global translator_trampoline
translator_trampoline:
    push {r0, r1, r2, lr}
    mov  r3, r8              @ low-mirror the dispatch table base
    subs r0, r1, r3           @ r0 = slotAddr - dispatchBase
    lsrs r0, r0, #3            @ r0 = idx — compileProc's 1st argument
    mov  r1, r9                @ r1 = runtime pointer — compileProc's 2nd argument
    REALIGN_ENTER                @ compileProc is real, independently-compiled C — see above
    bl   compileProc
    REALIGN_LEAVE
    pop  {r0, r1, r2}          @ restore acc, slotAddr, offset+1
    pop  {r3}                    @ r3 = the saved record; POP can't target lr directly
    mov  lr, r3
    ldr  r3, [r1, #0]          @ r3 = the code_ptr compileProc just wrote
    bx   r3

// uint64_t enter_dispatch(uint32_t argIn, Runtime *runtime)
//
// runtime_host.c's own entry into the ABI: an ordinary AAPCS function, not
// C inline asm — the raw BX-chain that follows (callHelper/the trampoline/
// compileProc/returnHelper/the prologue stub, none of it an ordinary AAPCS
// call) is entirely opaque to the C compiler either way, but expressed as
// a real function here it needs no clobber list, no `-ffixed-rN` fight
// over spare operand registers, no staging array — just r0-r1 in, and
// whatever's still live in r0-r3/r12 to use as scratch, exactly like any
// other hand-written leaf routine.
//
// Only two inputs now: `runtime` (r9 from here on) is the whole story —
// dispatchBase (r8) is `runtime + RUNTIME_DISPATCH_TABLE_OFFSET`, and
// there's no separate control-stack base to pass at all (call/return
// records travel in lr now, not on any stack — callHelper/returnHelper's
// own header, above). r10 (helper vector base) isn't a parameter either:
// g_helperVec (runtime_host.c, now 3 entries: callHelper,
// returnHelperFromLr, returnHelperFromStack) is link-time const, so it's
// loaded here by name — one fewer thing for the caller to compute.
//
// Result comes back as a `uint64_t` (r0 = value, r1 = trapped), not a
// 2-word struct: AAPCS64 would return such a struct in x0:x1, but AAPCS32
// only does that for composites *up to 4 bytes* — anything bigger goes
// through a hidden pointer, the exact hazard the old out-parameter design
// was built to dodge. A `uint64_t` is a "double-word integer," which
// AAPCS32 *does* return in r0:r1 directly, with nothing needing to survive
// the call in a register at all.
.thumb_func
.global enter_dispatch
enter_dispatch:
    push  {r2, r4, r5, r6, r7, lr} @ one push covers both: r2 is pure padding (8-byte alignment,
                                   @ AAPCS's own rule for every public interface — real even on
                                   @ Cortex-M0, which has no doubleword instructions of its own to
                                   @ care, since the requirement is architecture-family-wide, not
                                   @ per-core; confirmed empirically — arm-none-eabi-gcc pads the
                                   @ same way when it's short a register); r4-r7 are AAPCS
                                   @ callee-saved and genuinely live here: compiled procedures use
                                   @ them as their own window (src/window.ts), with zero obligation
                                   @ to preserve whatever the caller (enter_program) had in them —
                                   @ unlike the old inline-asm design, which told GCC about this
                                   @ via a clobber list, a real function has to actually honor the
                                   @ contract; lr is saved so the epilogue can end in a plain
                                   @ `pop {..., pc}`, same as any ordinary prologue/epilogue pair,
                                   @ rather than shuffling it through r3 to `bx` it.
    mov   r4, r8                   @ r8-r11 need the same treatment, mirrored through low
    mov   r5, r9                   @ registers first (ARMv6-M's compact PUSH/POP can't name
    mov   r6, r10                  @ r8-r11 directly). Nothing alive today happens to be sitting
    mov   r7, r11                  @ in them — runtime_host.c.o is built with -ffixed-r8/9/10/11,
    push  {r4, r5, r6, r7}         @ so nothing in that whole file ever keeps a live value there
                                   @ — but enter_dispatch is an ordinary AAPCS function called
                                   @ from enter_program, itself an ordinary AAPCS function called
                                   @ from plain, unconstrained C (main(), qemu-run-abi.ts's own
                                   @ generated program.c) — nothing enforces -ffixed-rN on *that*
                                   @ boundary, so the contract has to hold for real.
    mov   r9, r1                   @ NOW safe to set up the ABI's own dispatch context —
                                   @ runtime pointer
    movs  r2, r1
    adds  r2, #RUNTIME_DISPATCH_TABLE_OFFSET
    mov   r8, r2                   @ dispatch table base = runtime + RUNTIME_DISPATCH_TABLE_OFFSET
    ldr   r3, =g_helperVec
    mov   r10, r3                  @ static helper vector base
    movs  r3, #0
    mov   r11, r3                  @ LRU tick starts at 0
    mov   r3, sp
    str   r3, [r1, #0]             @ runtime->savedSp = sp (savedSp is runtime's own first field —
                                   @ bailOut's own restore point)
    subs  r2, r2, #DISPATCH_SENTINEL_OFFSET @ r2 = &dispatchTable[-1].code_ptr (still dispatchBase
                                   @ from above)
    ldr   r3, =.Lresume+1
    str   r3, [r2, #0]             @ ...gets this function's own landing address
    ldr   r1, =0xffff              @ boot record: proc_idx=-1, tag=0 (LANDING_SUCCESS)
    movs  r2, #0                   @ Q_idx = entry procedure (index 0)
    b     callHelper               @ tail-branch — callHelper never returns via bx lr
.Lresume:
    mov   r1, r2                   @ trapped = whatever tag came back (0 success / 1 LANDING_TRAP)
    pop   {r4, r5, r6, r7}         @ the caller's own r8-r11, still mirrored through low registers
    mov   r8, r4
    mov   r9, r5
    mov   r10, r6
    mov   r11, r7
    pop   {r2, r4, r5, r6, r7, pc} @ the caller's own real r4-r7, plus r2 discarded (padding)
                                   @ and pc <- lr — ordinary return, merged with the previous
                                   @ pop's counterpart push for the same reason: nothing happens
                                   @ between them that depends on splitting this into two
