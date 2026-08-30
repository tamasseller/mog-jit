.syntax unified
.cpu cortex-m0
.thumb

#include "dispatch_abi.h" // DISPATCH_SENTINEL_OFFSET / CALL_RECORD_BOOT / LANDING_*

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
    sxth  r1, r1                            @ r1 = callerIdx, SXTH: sentinel procIdx -1
    lsls  r1, r1, #4                        @ slotAddr-relative = callerIdx * 16 (sizeof(ProcSlot))
    add   r1, r1, r8                        @ r1 = slotAddr

    mov   r3, r11                           @ the LRU stamp, same as callHelper's — r3 is free
    str   r3, [r1, #4]                      @ until the codePtr load below. Deliberately NOT
    adds  r3, #1                            @ guarded against the sentinel: the whole point of
    mov   r11, r3                           @ slots[0] is that return-from-entry costs no extra code

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
    mov   r1, r8
    subs  r1, #DISPATCH_SENTINEL_OFFSET     @ r1 = &slots[0], the sentinel
    ldr   r3, [r1, #0]                      @ this excursion's landing
    ldr   r1, [r1, #8]                      @ ...and the sp it must land on
    movs  r2, #LANDING_TRAP
    mov   sp, r1
    bx    r3

.macro REALIGN_ENTER
    mov  r3, sp
    subs r2, r3, #4
    lsrs r2, r2, #3
    lsls r2, r2, #3
    mov  sp, r2
    str  r3, [sp, #0]
.endm

.macro REALIGN_LEAVE
    ldr  r3, [sp, #0]
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
    lsrs r0, r0, #4                         @ r0 = idx — translateProc's 1st argument (/16, sizeof(ProcSlot))
    mov  r1, r9                             @ r1 = runtime pointer — translateProc's 2nd argument
    REALIGN_ENTER                           @ translateProc is real, independently-compiled C — see above
    mov  r2, r11                            @ r2 = lruTick — 3rd argument, after REALIGN_ENTER's own use of r2
    bl   translateProc    
    REALIGN_LEAVE   
    pop  {r0, r1, r2}                       @ restore acc, slotAddr, offset+1
    pop  {r3}                               @ r3 = the saved record; POP can't target lr directly
    mov  lr, r3 
    ldr  r3, [r1, #0]                       @ r3 = the codePtr translateProc just wrote
    bx   r3

.thumb_func
.global enterDispatch   
enterDispatch:
    // r0: dispatch table
    // r1: runtime state
    // r2: argument block

    // Spill all callee saved regs and lr
    push  {r4, r5, r6, r7, lr}          
    mov   r4, r8                            
    mov   r5, r9                            
    mov   r6, r10                           
    mov   r7, r11                           
    push  {r4, r5, r6, r7} 

    // Setup dispatch table hi-reg
    mov   r8, r0

    // Setup return from entry and trap/failure
    // Landing in codePtr, so that returnHelperTail finds it.
    // SP goes in the slot's tail, which no dispatch path writes.
    mov   r4, r0
    subs  r4, r4, #DISPATCH_SENTINEL_OFFSET 
    ldr   r5, =.Lresume+1                   
    str   r5, [r4, #0]                      
    mov   r5, sp
    str   r5, [r4, #8]

    // Setup runtime hireg
    mov   r9, r1

    // Setup helper vector
    ldr   r4, =helperVec
    mov   r10, r4

    // Initialize LRU tick counter
    movs  r4, #0
    mov   r11, r4

    // Copy spilled args
    ldmia r2!, {r4} //  EntryArgs::spilledStart
    ldmia r2!, {r5} //  EntryArgs::spilledEnd

.LentrySpill:
    cmp   r4, r5
    beq   .LentryWindow
    ldmia r4!, {r1}
    push  {r1}
    b     .LentrySpill
.LentryWindow:                            

    // Load entry window
    ldmia r2!, {r0, r4, r5, r6, r7} // EntryArgs::acc, EntryArgs::window

    // Parameters for call helper, callee: #0, return to #-1 @ offset 0
    ldr   r1, =CALL_RECORD_BOOT
    movs  r2, #0
    b     callHelper

    // Resumption after bailout or regular return.
.Lresume:
    mov   r1, r2

    // Fill all callee_saved regs and load the stored lr into pc for return
    pop   {r4, r5, r6, r7}                  
    mov   r8, r4
    mov   r9, r5
    mov   r10, r6
    mov   r11, r7
    pop   {r4, r5, r6, r7, pc}