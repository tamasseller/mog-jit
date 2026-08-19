/**
 * @ppl/jit-armv6m-prototype — register assignment (docs/jit-armv6m.md §3)
 *
 * One file, one doc section: every fixed physical-register role this
 * prototype uses, gathered in one place instead of window.ts/accstate.ts
 * each picking their own register numbers ad hoc. Only the roles this
 * scope actually needs — §3's r8-r11 (dispatch-table/arena base pointers)
 * have no counterpart here, since nothing does `CALL`/eviction yet.
 */

export const ACC_REG = 0 // r0 — acc (docs/jit-armv6m.md §3: matches AAPCS's own
                          // first-argument/return-value register, so a single-
                          // argument native helper call needs no shuffle at all)

/** Scratch — materializing an immediate operand ahead of an instruction
 *  that has no immediate form, or the rare imm-op-imm degenerate case.
 *  Never acc's own home, never a window register. */
export const SCRATCH_REG = 2 // r2

export const WINDOW_BASE = 4 // r4
export const WINDOW_SIZE = 4 // r4..r7

// Real dispatch/call-return ABI (docs/jit-armv6m-dispatch-handoff.html §01) —
// only used by the ABI-real translateProc strategy (runtime.ts/programAbi.ts),
// not the default no-eviction path above.

/** Entry ABI — index/slot-address, into `callHelper`/`returnHelper`/the
 *  prologue stub/the translator trampoline. Same physical register as
 *  `SCRATCH_REG` isn't a concern here — disjoint lifetimes, never both live. */
export const ENTRY_IDX_REG = 1 // r1
/** Entry ABI — offset+1 (Thumb-bit pre-folded in, §07). */
export const ENTRY_OFFSET_REG = SCRATCH_REG // r2
/** Entry ABI — jump target, dead the instant control lands. */
export const ENTRY_JUMP_REG = 3 // r3

export const DISPATCH_BASE_REG = 8  // r8 — dispatch table base (info block at negative offsets, §09)
export const CONTROL_SP_REG = 9     // r9 — control stack pointer
export const HELPER_VEC_REG = 10    // r10 — static helper vector base
export const LRU_TICK_REG = 11      // r11 — monotonic LRU tick counter
