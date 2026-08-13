/**
 * @ppl/jit-armv6m-prototype — register assignment (docs/jit-armv6m.md §3)
 *
 * One file, one doc section: every fixed physical-register role this
 * prototype uses, gathered in one place instead of window.ts/accstate.ts
 * each picking their own register numbers ad hoc. Only the roles this
 * scope actually needs — §3's r8-r11 (dispatch-table/arena base pointers)
 * have no counterpart here, since nothing does `CALL`/eviction yet.
 */

export const ACC_REG = 3 // r3 — acc

/** Scratch — materializing an immediate operand ahead of an instruction
 *  that has no immediate form, or the rare imm-op-imm degenerate case.
 *  Never acc's own home, never a window register. */
export const SCRATCH_REG = 2 // r2

export const WINDOW_BASE = 4 // r4
export const WINDOW_SIZE = 4 // r4..r7
