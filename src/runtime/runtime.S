.syntax unified
.cpu cortex-m0
.thumb

#include "dispatch_abi.h" // DISPATCH_SENTINEL_OFFSET / CALL_RECORD_BOOT / LANDING_*

/*
 * r0: acc to pass
 * r1: return record (caller proc idx & offset)
 * r2: callee proc idx
 */
.thumb_func
.global callHelper
callHelper:
    // Store return record in LR right away (frees r1 as scratch)
    mov   lr, r1

    // Load the jump target address from dispatch table into r3 (also keep slot base in r1)
    lsls  r1, r2, #4
    add   r1, r1, r8
    ldr   r3, [r1, #0]

    // LRU bump
    mov   r2, r11
    str   r2, [r1, #4]
    adds  r2, #1
    mov   r11, r2

    // Set offset for entry (thumb bit) and enter, prolog stub jumps to PC+r2
    movs  r2, #1
    bx    r3

/*
 * r0: acc to pass
 * LR: return record (caller proc idx & offset)
 *
 * Return from leaf case (LR stayed put).
 */
.thumb_func
.global returnHelperFromLr
returnHelperFromLr:                         @ leaf procedures: the record has sat untouched in lr since entry
    mov   r1, lr
    b     returnHelperTail

/*
 * r0: acc to pass
 * tos has return record but no window restoration is needed after
 *
 * Return from non-leaf with no spilled args
 */
.thumb_func
.global returnHelperFromStack
returnHelperFromStack:                      @ ordinary non-leaf procedures: retrieve what their own prologue pushed
    pop   {r1}                              @ r1 = packed record

/*
 * r0: acc to passed
 * r1: return record (caller proc idx & offset)
 */
.thumb_func
returnHelperTail:                           @ not in the helper vector: only ever reached from within this file
    // Split return record, offset in r2, proc idx in r1
    lsrs  r2, r1, #16
    sxth  r1, r1

    // Calculate slot base from proc idx into r1 
    lsls  r1, r1, #4
    add   r1, r1, r8

    // LRU bump
    mov   r3, r11
    str   r3, [r1, #4]
    adds  r3, #1
    mov   r11, r3

    // Load target address and enter (r2 already has the offset for the PC+r2 jump)
    ldr   r3, [r1, #0]
    bx    r3

/*
 * r0: acc to pass
 * r2: the stack fixed needed after return record load
 * tos has return record and spilled args behind it
 *
 * Return from non-leaf with spilled args (can not be cleaned 
 * up earlier due to the return record being in the way).
 */
.thumb_func
.global returnHelperFromStackReclaim
returnHelperFromStackReclaim:               
    pop   {r1}                              
    add   sp, sp, r2                        
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

.thumb_func
.global extThunkHelper
extThunkHelper:
    // Spill LR and store original SP in LR for restore point later
    push {lr}
    mov  lr, sp

    // Realign SP to 8 for AAPCS callee is C code
    mov  r3, sp
    subs r3, #4
    lsrs r3, r3, #3
    lsls r3, r3, #3
    mov  sp, r3

    // Store original SP but leave it in r3 for four arg
    mov  r3, lr
    str  r3, [sp, #0]
    adds r3, #4

    // Execute target as per AAPCS
    blx  r12

    // Restore stack and return
    ldr  r3, [sp, #0]
    mov  sp, r3
    pop  {pc}

.thumb_func
.global translatorTrampoline
translatorTrampoline:
    // Spill original args and LR
    push {r0, r1, r2, lr}

    // Calculate target proc idx (so that translateProc doesn't need to handle dispatchBase in hi-reg)
    mov  r3, r8
    subs r0, r1, r3
    lsrs r0, r0, #4

    // Fetch runtime pointer from r9
    mov  r1, r9

    // Realign SP to 8 for AAPCS callee is C code
    mov  r3, sp
    subs r2, r3, #4
    lsrs r2, r2, #3
    lsls r2, r2, #3
    mov  sp, r2

    // Store original SP at new TOS
    str  r3, [sp, #0]

    // Fetch lruTick from r11
    mov  r2, r11

    // Enter the compiler
    bl   translateProc    

    // Restore SP
    ldr  r3, [sp, #0]
    mov  sp, r3

    // Restore original args and LR (via r3, because Thumb-1)
    pop  {r0, r1, r2}
    pop  {r3}
    mov  lr, r3 

    // Continue to call entry helper
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