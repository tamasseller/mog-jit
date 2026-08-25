// The fixed, flash-resident runtime machinery: callHelper, returnHelper,
// the translator-entry trampoline, and enterDispatch. All four sit at one
// link-time-fixed address for the whole program's lifetime, reached via the
// r10 helper vector (the first two), a dispatch slot's initial codePtr (the
// trampoline), or an ordinary bl from runtime_host.cpp (enterDispatch) —
// never copied anywhere, unlike the per-procedure prologue stub, which is
// copied into the arena as the first bytes of every compiled procedure.
// Hand-written and built by the normal toolchain, same as
// ../src/vectors.S's own reset handler.

.syntax unified
.cpu cortex-m0
.thumb

#include "runtime_host.h" // RUNTIME_DISPATCH_TABLE_OFFSET / DISPATCH_SENTINEL_OFFSET

// in: r1 = packed record (P_idx | K+1<<16), r2 = Q_idx
// out: never returns — tail-jumps into Q's resident code or the translator
//
// The record travels in lr from here on, not on the operand stack: this
// JIT never emits hardware BL/BLX in its own compiled code, so lr is
// otherwise dead. A callee that itself makes a nested CALL saves this word
// to its own stack in its own prologue instead, once per activation — this
// routine (and returnHelper, below) carries the record's handoff exactly
// once, in flash, never duplicated per call/return site.
.thumb_func
.global callHelper
callHelper:
    mov   lr, r1               @ persist the record in lr — r1 is free again immediately after
    lsls  r1, r2, #4          @ slotAddr-relative = Q_idx * 16 (sizeof(ProcSlot))
    add   r1, r1, r8           @ r1 = slotAddr
    ldr   r3, [r1, #0]          @ r3 = codePtr
    movs  r2, #1                 @ offset+1 = 1, hardwired — a fresh call never resumes mid-procedure
    bx    r3

// Four entry points feeding one shared tail: which one a procedure's own
// RETURN/TRAP dispatches to depends on whether that procedure's own
// prologue pushed {lr}, and — if it did — whether it also has any original
// out-of-window arguments (argCount > WINDOW_SIZE) sitting below that
// pushed word, needing an extra reclaim no parameterless routine can size
// on its own. That size (in bytes) travels in r2, loaded by the call site
// (abi_strategy.cpp's abiEmitReturn) only on the one variant that needs it.
//
// in: nothing (returnHelperFromLr) / the caller's own record already
// pushed by this procedure's own prologue (returnHelperFromStack), same
// plus r2 = the reclaim byte count (returnHelperFromStackReclaim) / r1
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
    sxth  r1, r1                  @ r1 = callerIdx, sign-extended — SXTH not UXTH: the
                                    @ sentinel procIdx (0xffff, "index -1") must resolve
                                    @ to r8-16, not r8+1048560
    lsls  r1, r1, #4                 @ slotAddr-relative = callerIdx * 16 (sizeof(ProcSlot))
    add   r1, r1, r8                  @ r1 = slotAddr
    ldr   r3, [r1, #0]                 @ r3 = codePtr
    bx    r3

.thumb_func
.global returnHelperFromStackReclaim
returnHelperFromStackReclaim:  @ non-leaf, with out-of-window arguments below the pushed record
    pop   {r1}                @ r1 = packed record
    add   sp, sp, r2          @ reclaim what discardWindow deliberately left for us — r2 set by the call site
    b     returnHelperTail

// The reserved software-helper slots: CLZ/REVBITS have no ARMv6-M native
// form at all, and BR_TABLE(N>2) needs a genuine multi-way dispatch no
// fused conditional branch can express. One flash-resident copy of each,
// reached the same way as callHelper/returnHelper* (MOV r3,r10 / LDR
// r3,[r3,#idx*4] / BLX r3) — BLX rather than BX even for brTableJumpHelper's
// own tail-jump, since the call site's own jump table sits immediately
// after that BLX and this routine locates it through lr.

// in: r0 = acc (operand, in place). out: r0 = acc (result), never traps.
// Clobbers r1 only. Reached via BLX — an ordinary subroutine call, unlike
// the tail-jumping helpers above.
.thumb_func
.global clzHelper
clzHelper:
    movs  r1, #0
    cmp   r0, #0
    beq   .LclzZero
.LclzLoop:
    lsls  r0, r0, #1            @ carry = the bit shifted out, i.e. the old bit 31
    bhs   .LclzDone              @ carry set == that bit was 1
    adds  r1, #1
    b     .LclzLoop
.LclzDone:
    mov   r0, r1
    bx    lr
.LclzZero:
    movs  r0, #32
    bx    lr

// in: r0 = acc. out: r0 = acc. Clobbers r1/r2 (SCRATCH_REG) — never a
// window register or acc's own home, so nothing at the call site needs
// saving around this.
.thumb_func
.global revbitsHelper
revbitsHelper:
    mov   r1, r0
    movs  r0, #0
    movs  r2, #32
.LrevbitsLoop:
    lsrs  r1, r1, #1            @ carry = the bit shifted out, i.e. the old bit 0
    adcs  r0, r0                 @ r0 = r0 + r0 + carry — double the result, fold the bit in at the bottom
    subs  r2, #1
    beq   .LrevbitsDone
    b     .LrevbitsLoop
.LrevbitsDone:
    bx    lr

// in: r0 = selector, r2 (SCRATCH_REG) = N (the clamp ceiling, one past the
// last real case — set by the call site right before the BLX that reaches
// here), lr = this call's own return address, which is exactly where the
// caller's n+1-halfword jump table starts.
// out: never returns — tail-jumps into whichever case body the table
// names, or the out-of-range slot at index N. Clobbers r1/r2/r3.
.thumb_func
.global brTableJumpHelper
brTableJumpHelper:
    mov   r1, r0                @ default: in range, index = selector
    cmp   r0, r2
    bls   .LbrtOk
    mov   r1, r2                 @ out of range: index = N — r0 untouched either way
.LbrtOk:
    lsls  r1, r1, #1             @ halfword-indexed
    mov   r3, lr                  @ lr -> low reg, for LDRH's addressing
    // BLX always sets lr with bit 0 forced to 1 (the Thumb-mode marker) —
    // fatal for LDRH's own address arithmetic (needs a halfword-aligned
    // address), so clear it for the table lookup, then restore it right
    // before the final BX.
    subs  r3, #1
    ldrh  r2, [r3, r1]
    adds  r3, r3, r2
    adds  r3, #1                  @ restore the Thumb-mode bit for the final BX
    bx    r3

// Realign sp down to an 8-byte boundary right before handing off to
// independently-compiled C (the translator today, and eventually any
// C-implemented extension helper reached through the r10 vector). Nothing
// in this file's own hand-written excursion ever needs the AAPCS 8-byte-SP
// guarantee, since none of it is independently compiled — but the callee on
// the other side of a real BL/BLX might be, and might use LDRD/STRD or
// simply assume its own locals land 8-aligned the way AAPCS promises every
// caller will leave them.
//
// Branchless, and doesn't care whether sp happens to already be aligned:
// always reserves a full 8 bytes below the true sp, rounds down, then
// stashes the real sp at a fixed +4 offset into that slack rather than
// pushing it — a push after rounding down would just re-misalign sp for the
// call that follows. Clobbers r2/r3 only.
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
// codePtr points at. Reached by a bare BX from callHelper/returnHelper/the
// prologue stub, exactly like any other slot target: r0 = acc (must
// survive), r1 = slotAddr, r2 = offset+1 (must survive). The one piece of
// this file that genuinely needs to be hand-written: it makes a real call
// into compileProc, whose address isn't known until link time.
//
// r0/r1/r2 round-trip mirrored across the call — no per-register reasoning
// needed. r8/r9/r10/r11 need no saving here: they're AAPCS callee-saved,
// and compileProc is built with -ffixed-r8/r9/r10/r11 so the compiler can't
// touch them at all. lr is the odd one out: it holds whatever call/return
// record was live when this trampoline was reached, and `bl compileProc`
// clobbers it same as any ordinary call — saved and restored through r3
// since POP has no form that targets lr directly.
.thumb_func
.global translatorTrampoline
translatorTrampoline:
    push {r0, r1, r2, lr}
    mov  r3, r8              @ low-mirror the dispatch table base
    subs r0, r1, r3           @ r0 = slotAddr - dispatchBase
    lsrs r0, r0, #4            @ r0 = idx — compileProc's 1st argument (/16, sizeof(ProcSlot))
    mov  r1, r9                @ r1 = runtime pointer — compileProc's 2nd argument
    REALIGN_ENTER                @ compileProc is real, independently-compiled C — see above
    bl   compileProc
    REALIGN_LEAVE
    pop  {r0, r1, r2}          @ restore acc, slotAddr, offset+1
    pop  {r3}                    @ r3 = the saved record; POP can't target lr directly
    mov  lr, r3
    ldr  r3, [r1, #0]          @ r3 = the codePtr compileProc just wrote
    bx   r3

// uint64_t enterDispatch(uint32_t argIn, Runtime *runtime)
//
// runtime_host.cpp's own entry into the ABI: an ordinary AAPCS function,
// not C inline asm — the raw BX-chain that follows (callHelper/the
// trampoline/compileProc/returnHelper/the prologue stub, none of it an
// ordinary AAPCS call) is entirely opaque to the C compiler either way, but
// expressed as a real function here it needs no clobber list and no
// staging array.
//
// Only two inputs: runtime (r9 from here on) is the whole story —
// dispatchBase (r8) is runtime + RUNTIME_DISPATCH_TABLE_OFFSET, and there's
// no separate control-stack base to pass at all. r10 (helper vector base)
// isn't a parameter either: helperVec (runtime_host.cpp, 8 entries:
// callHelper, returnHelperFromLr, returnHelperFromStack, returnHelperTail,
// clzHelper, revbitsHelper, brTableJumpHelper, returnHelperFromStackReclaim)
// is link-time const, so it's loaded here by name.
//
// Result comes back as a uint64_t (r0 = value, r1 = trapped), not a 2-word
// struct: AAPCS32 returns composites over 4 bytes through a hidden pointer,
// but a uint64_t is a double-word integer, which AAPCS32 returns in r0:r1
// directly.
.thumb_func
.global enterDispatch
enterDispatch:
    push  {r2, r4, r5, r6, r7, lr} @ one push covers both: r2 is pure padding (8-byte
                                   @ alignment, AAPCS's own rule for every public
                                   @ interface); r4-r7 are AAPCS callee-saved and
                                   @ genuinely live here: compiled procedures use them as
                                   @ their own window, with zero obligation to preserve
                                   @ whatever the caller (enterProgram) had in them; lr is
                                   @ saved so the epilogue can end in a plain
                                   @ `pop {..., pc}`.
    mov   r4, r8                   @ r8-r11 need the same treatment, mirrored through low
    mov   r5, r9                   @ registers first (ARMv6-M's compact PUSH/POP can't
    mov   r6, r10                  @ name r8-r11 directly). enterDispatch is an ordinary
    mov   r7, r11                  @ AAPCS function called from enterProgram, itself
    push  {r4, r5, r6, r7}         @ called from plain, unconstrained C — nothing enforces
                                   @ -ffixed-rN on that boundary, so the contract has to
                                   @ hold for real.
    mov   r9, r1                   @ NOW safe to set up the ABI's own dispatch context —
                                   @ runtime pointer
    movs  r2, r1
    adds  r2, #RUNTIME_DISPATCH_TABLE_OFFSET
    mov   r8, r2                   @ dispatch table base = runtime + RUNTIME_DISPATCH_TABLE_OFFSET
    ldr   r3, =helperVec
    mov   r10, r3                  @ static helper vector base
    movs  r3, #0
    mov   r11, r3                  @ LRU tick starts at 0
    mov   r3, sp
    str   r3, [r1, #0]             @ runtime->savedSp = sp (savedSp is runtime's own first
                                   @ field — bailOut's own restore point)
    subs  r2, r2, #DISPATCH_SENTINEL_OFFSET @ r2 = &dispatchTable[-1].codePtr (still
                                   @ dispatchBase from above)
    ldr   r3, =.Lresume+1          @ the +1 is this file's own hand-written analog of
                                   @ setCodePtr — .Lresume is a bare local label, not
                                   @ .thumb_func-tagged, so nothing else supplies the
                                   @ Thumb bit here
    str   r3, [r2, #0]             @ ...gets this function's own landing address
    ldr   r1, =0xffff              @ boot record: procIdx=-1, tag=0 (LANDING_SUCCESS)
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
                                   @ and pc <- lr
