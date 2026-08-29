.syntax unified
.cpu cortex-m0
.thumb

#include "runtime_host.h" // RUNTIME_DISPATCH_TABLE_OFFSET / DISPATCH_SENTINEL_OFFSET / ENTRY_ARGS_*_OFFSET

.thumb_func
.global callHelper
callHelper:
    mov   lr, r1                            @ persist the record in lr — r1 is free again immediately after
    lsls  r1, r2, #4                        @ slotAddr-relative = Q_idx * 16 (sizeof(ProcSlot))
    add   r1, r1, r8                        @ r1 = slotAddr
    ldr   r3, [r1, #0]                      @ r3 = codePtr
    mov   r2, r11                           @ low-mirror the LRU tick (STR has no hi form) — r2 is dead
    str   r2, [r1, #4]                      @ from the lsls above until the movs below, so it costs no save
    adds  r2, #1
    mov   r11, r2                           @ publish the bumped tick
    movs  r2, #1                            @ offset+1 = 1, hardwired — a fresh call never resumes mid-procedure
    bx    r3

.thumb_func
.global returnHelperFromLr
returnHelperFromLr:                         @ leaf procedures: the record has sat untouched in lr since entry
    mov   r1, lr
    b     returnHelperTail

.thumb_func
.global returnHelperFromStack
returnHelperFromStack:                      @ ordinary non-leaf procedures: retrieve what their own prologue pushed
    pop   {r1}                              @ r1 = packed record

.thumb_func
.global returnHelperTail
returnHelperTail:
    lsrs  r2, r1, #16                       @ r2 = offset+1
    sxth  r1, r1                            @ r1 = callerIdx, sign-extended — SXTH not UXTH: the
                                            @ sentinel procIdx (0xffff, "index -1") must resolve
                                            @ to r8-16, not r8+1048560
    lsls  r1, r1, #4                        @ slotAddr-relative = callerIdx * 16 (sizeof(ProcSlot))
    add   r1, r1, r8                        @ r1 = slotAddr
    mov   r3, r11                           @ the LRU stamp, same as callHelper's — r3 is free
    str   r3, [r1, #4]                      @ until the codePtr load below. Deliberately NOT
    adds  r3, #1                            @ guarded against the sentinel: the whole point of
    mov   r11, r3                           @ slots[0] is that return-from-entry costs no extra
                                            @ code, and the word it lands on is the sentinel's
                                            @ own lastUsed, which nothing reads (the eviction
                                            @ scan runs over slot(i) == slots[i+1]) and no
                                            @ extension owns (RUNTIME_EXT_STATE_OFFSET starts
                                            @ one field later, at bodyPtr).
    ldr   r3, [r1, #0]                      @ r3 = codePtr
    bx    r3

.thumb_func
.global returnHelperFromStackReclaim
returnHelperFromStackReclaim:               @ non-leaf, with out-of-window arguments below the pushed record
    pop   {r1}                              @ r1 = packed record
    add   sp, sp, r2                        @ reclaim what discardWindow deliberately left for us — r2 set by the call site
    b     returnHelperTail

.thumb_func
.global clzHelper
clzHelper:
    movs  r1, #0
    cmp   r0, #0
    beq   .LclzZero
.LclzLoop:
    lsls  r0, r0, #1                        @ carry = the bit shifted out, i.e. the old bit 31
    bhs   .LclzDone                         @ carry set == that bit was 1
    adds  r1, #1
    b     .LclzLoop
.LclzDone:
    mov   r0, r1
    bx    lr
.LclzZero:
    movs  r0, #32
    bx    lr

.thumb_func
.global revbitsHelper
revbitsHelper:
    mov   r1, r0
    movs  r0, #0
    movs  r2, #32
.LrevbitsLoop:
    lsrs  r1, r1, #1                        @ carry = the bit shifted out, i.e. the old bit 0
    adcs  r0, r0                            @ r0 = r0 + r0 + carry — double the result, fold the bit in at the bottom
    subs  r2, #1
    beq   .LrevbitsDone
    b     .LrevbitsLoop
.LrevbitsDone:
    bx    lr

.thumb_func
.global brTableJumpHelper
brTableJumpHelper:
    mov   r1, r0                            @ default: in range, index = selector
    cmp   r0, r2
    bls   .LbrtOk
    mov   r1, r2                            @ out of range: index = N — r0 untouched either way
.LbrtOk:
    lsls  r1, r1, #1                        @ halfword-indexed
    mov   r3, lr                            @ lr -> low reg, for LDRH's addressing
    subs  r3, #1
    ldrh  r2, [r3, r1]
    adds  r3, r3, r2
    adds  r3, #1                            @ restore the Thumb-mode bit for the final BX
    bx    r3

.thumb_func
.global trapHelper
trapHelper:
    mov   r1, r9                            @ r1 = runtime
    ldr   r3, [r1, #(RUNTIME_DISPATCH_TABLE_OFFSET - DISPATCH_SENTINEL_OFFSET)] @ = slots[0].codePtr,
                                            @ the sentinel landing enterDispatch parked below — reached from r9
                                            @ directly rather than via r8, so this needs one live register, not two
    ldr   r1, [r1, #0]                      @ r1 = runtime->savedSp (savedSp is Runtime's own first field)
    movs  r2, #LANDING_TRAP                 @ the landing reads its own tag out of r2
    mov   sp, r1
    bx    r3

.macro REALIGN_ENTER
    mov  r3, sp
    lsrs r2, r3, #3
    lsls r2, r2, #3                         @ r2 = old sp rounded down to 8 (Rd!=Rn shifts have no 3-bit-imm limit)
    subs r2, r2, #8                         @ Rd==Rn here, so the 8-bit-immediate SUBS form applies (unlike Rd!=Rn, which caps at 3 bits)
    mov  sp, r2
    str  r3, [sp, #4]
.endm

.macro REALIGN_LEAVE
    ldr  r3, [sp, #4]
    mov  sp, r3
.endm

.thumb_func
.global extThunkHelper
extThunkHelper:
    push {lr}
    REALIGN_ENTER
    blx  r12
    REALIGN_LEAVE
    pop  {pc}

.thumb_func
.global translatorTrampoline
translatorTrampoline:
    push {r0, r1, r2, lr}
    mov  r3, r8                             @ low-mirror the dispatch table base
    subs r0, r1, r3                         @ r0 = slotAddr - dispatchBase
    lsrs r0, r0, #4                         @ r0 = idx — compileProc's 1st argument (/16, sizeof(ProcSlot))
    mov  r1, r9                             @ r1 = runtime pointer — compileProc's 2nd argument
    REALIGN_ENTER                           @ compileProc is real, independently-compiled C — see above
    bl   compileProc    
    REALIGN_LEAVE   
    pop  {r0, r1, r2}                       @ restore acc, slotAddr, offset+1
    pop  {r3}                               @ r3 = the saved record; POP can't target lr directly
    mov  lr, r3 
    ldr  r3, [r1, #0]                       @ r3 = the codePtr compileProc just wrote
    bx   r3

.thumb_func
.global enterDispatch
enterDispatch:
    push  {r2, r4, r5, r6, r7, lr}          @ one push covers both: r2 is pure padding (8-byte
                                            @ alignment, AAPCS's own rule for every public
                                            @ interface); r4-r7 are AAPCS callee-saved and
                                            @ genuinely live here: compiled procedures use them as
                                            @ their own window, with zero obligation to preserve
                                            @ whatever the caller (enterProgram) had in them; lr is
                                            @ saved so the epilogue can end in a plain
                                            @ `pop {..., pc}`.
    mov   r4, r8                            @ r8-r11 need the same treatment, mirrored through low
    mov   r5, r9                            @ registers first (ARMv6-M's compact PUSH/POP can't
    mov   r6, r10                           @ name r8-r11 directly). enterDispatch is an ordinary
    mov   r7, r11                           @ AAPCS function called from enterProgram, itself
    push  {r4, r5, r6, r7}                  @ called from plain, unconstrained C — nothing enforces
                                            @ -ffixed-rN on that boundary, so the contract has to
                                            @ hold for real.
    mov   r9, r1                            @ NOW safe to set up the ABI's own dispatch context —
                                            @ runtime pointer
    movs  r2, r1
    adds  r2, #RUNTIME_DISPATCH_TABLE_OFFSET
    mov   r8, r2                            @ dispatch table base = runtime + RUNTIME_DISPATCH_TABLE_OFFSET
    ldr   r3, =helperVec
    mov   r10, r3                           @ static helper vector base
    movs  r3, #0
    mov   r11, r3                           @ LRU tick starts at 0
    mov   r3, sp
    str   r3, [r1, #0]                      @ runtime->savedSp = sp (savedSp is runtime's own first
                                            @ field — bailOut's own restore point)
    subs  r2, r2, #DISPATCH_SENTINEL_OFFSET @ r2 = &dispatchTable[-1].codePtr (still
                                            @ dispatchBase from above)
    ldr   r3, =.Lresume+1                   @ the +1 is this file's own hand-written analog of
                                            @ setCodePtr — .Lresume is a bare local label, not
                                            @ .thumb_func-tagged, so nothing else supplies the
                                            @ Thumb bit here
    str   r3, [r2, #0]                      @ ...gets this function's own landing address

    ldr   r2, [r0, #ENTRY_ARGS_SPILLED_OFFSET]       @ &args[0]
    ldr   r3, [r0, #ENTRY_ARGS_SPILLED_COUNT_OFFSET] @ max(0, arg_count - WINDOW_SIZE)
    cmp   r3, #0                            @ no CBZ on ARMv6-M, so this guard is real work
    beq   .LentryWindow
.LentrySpill:                               @ ascending slot order, so slot 0 ends up
    ldr   r1, [r2, #0]                      @ furthest from sp — what rawSpillOffset expects
    push  {r1}
    adds  r2, #4
    subs  r3, #1
    bne   .LentrySpill
.LentryWindow:                              @ the rotation by arg_count mod WINDOW_SIZE already
    ldr   r4, [r0, #(ENTRY_ARGS_WINDOW_OFFSET + 0)]  @ happened in C — window[i] is
    ldr   r5, [r0, #(ENTRY_ARGS_WINDOW_OFFSET + 4)]  @ addressed to WINDOW_BASE + i, so
    ldr   r6, [r0, #(ENTRY_ARGS_WINDOW_OFFSET + 8)]  @ these four are a straight copy
    ldr   r7, [r0, #(ENTRY_ARGS_WINDOW_OFFSET + 12)]
    ldr   r0, [r0, #ENTRY_ARGS_ACC_OFFSET] @ acc last: it overwrites the descriptor pointer

    ldr   r1, =0xffff                       @ boot record: procIdx=-1, tag=0 (LANDING_SUCCESS)
    movs  r2, #0                            @ Q_idx = entry procedure (index 0)
    b     callHelper                        @ tail-branch — callHelper never returns via bx lr
.Lresume:
    mov   r1, r2                            @ trapped = whichever LANDING_* tag came back (runtime_host.h):
                                            @ 0 from the boot record's own zero resume-offset field on an
                                            @ ordinary return, 1 from trapHelper, 2 from runtimeBail
    pop   {r4, r5, r6, r7}                  @ the caller's own r8-r11, still mirrored through low registers
    mov   r8, r4
    mov   r9, r5
    mov   r10, r6
    mov   r11, r7
    pop   {r2, r4, r5, r6, r7, pc}          @ the caller's own real r4-r7, plus r2 discarded (padding)
                                            @ and pc <- lr
