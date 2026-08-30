/* MEMMOVE's inner loop, as hand-written Thumb reached through
 * ExtSite::helperCall — the raw form, so no AAPCS applies and the clobber
 * set is this file's own business (design.md §18.3).
 *
 * r0 = source offset, r1 = destination start, r2 = destination end, all
 * masked to the buffer by the emitted code. The destination is turned into
 * a pointer pair and walked until it reaches the end, so a range whose end
 * is at or below its start copies nothing — that is the whole handling of
 * a nonsensical argument. The source stays an offset and wraps, which is
 * what keeps it inside the buffer without a second range to check.
 *
 * r3 is free on entry: it held this routine's own address for the BLX. The
 * end pointer lives in r12, the AAPCS intra-procedure scratch register,
 * because CMP is one of the three things Thumb-1 lets a hi register do —
 * which keeps the base in a low register and the loop free of any
 * per-iteration register shuffling. r4-r11 belong to the translator's
 * window and runtime ABI and are never touched; lr is already saved by the
 * procedure prologue, which is what EXT_FLAG_NEEDS_LR arranges, so this
 * costs no stack at all. */

    .syntax unified
    .cpu cortex-m0
    .thumb

    .section .text
    .thumb_func
    .global rawMemMoveHelper
rawMemMoveHelper:
    ldr   r3, .LrawMemBase
    adds  r2, r2, r3                @ one past the last byte written
    mov   r12, r2                   @ CMP is one of the few that reaches a hi reg
    adds  r1, r1, r3                @ destination cursor
    mov   r2, r3                    @ base, for the indexed source load

.LcopyLoop:
    cmp   r1, r12
    bhs   .LcopyDone                @ unsigned: end <= start copies nothing
    ldrb  r3, [r2, r0]
    strb  r3, [r1]
    adds  r1, #1
    adds  r0, #1
    lsls  r0, r0, #22               @ source offset wraps at the buffer size
    lsrs  r0, r0, #22
    b     .LcopyLoop

.LcopyDone:
    bx    lr

    .align 2
.LrawMemBase:
    .word g_rawMem
