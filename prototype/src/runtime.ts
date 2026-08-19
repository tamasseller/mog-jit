/**
 * @ppl/jit-armv6m-prototype — the fixed, position-independent runtime
 * machinery (docs/jit-armv6m-dispatch-handoff.html §04-§06/§09), TS-emitted
 * via armv6.ts like everything else this prototype generates.
 *
 * Only the pieces that ever get *copied* to a dynamic runtime address live
 * here: the per-procedure prologue stub (copied into the arena ahead of
 * every compiled procedure's own body, §06) and `callHelper`/`returnHelper`
 * (flash-resident but self-contained — no absolute addresses, no `BL` to
 * anything external, so nothing stops them being emitted the same way).
 * The translator-entry trampoline is deliberately *not* here — it needs a
 * real, linker-resolved `bl` into the mock translator's C body, which a
 * hand-encoded blob can't express; see qemu/trampoline.S.
 */

import * as arm from "./armv6"
import {
    ENTRY_IDX_REG, ENTRY_OFFSET_REG, ENTRY_JUMP_REG,
    DISPATCH_BASE_REG, CONTROL_SP_REG, HELPER_VEC_REG, LRU_TICK_REG,
} from "./registers"

const PC = 15

/**
 * The first instructions of every compiled procedure (§06's "per-procedure
 * prologue stub"): touch the LRU tick, then resolve `r2` (offset+1) against
 * this instruction's own address into a real branch target and tail-jump —
 * never a data-processing write to `pc` (§07), always `ADD`-then-`BX`.
 *
 * Fixed size, and that size is load-bearing: `ADD r2,r2,pc`'s own PC-read
 * (address-of-this-instruction + 4) lands exactly on the first byte past
 * this stub, which is *by construction* the same as this template's own
 * total length (one instruction, the `BX`, follows the `ADD`) — so
 * `offset=0` (a fresh call, §06) resolves to exactly "body start," and any
 * other `offset` is a byte distance measured from that same point, not from
 * the procedure's absolute start (translateProc.ts's `STUB_SIZE` subtraction
 * when capturing a `CALL` site's own resume offset).
 */
export function emitPrologueStub(): number[]
{
    const code = [
        arm.movHi(ENTRY_JUMP_REG, LRU_TICK_REG),      // MOV r3, r11 — low-mirror the LRU tick
        arm.str5(ENTRY_JUMP_REG, ENTRY_IDX_REG, 4),   // STR r3, [r1, #4] — entry.last_used = old tick
        arm.addsImm8(ENTRY_JUMP_REG, 1),              // ADDS r3, r3, #1
        arm.movHi(LRU_TICK_REG, ENTRY_JUMP_REG),      // MOV r11, r3 — publish the bumped tick
        arm.addHi(ENTRY_OFFSET_REG, PC),              // ADD r2, r2, pc
        arm.bx(ENTRY_OFFSET_REG),                     // BX r2
    ]
    return code
}

export const STUB_SIZE = emitPrologueStub().length * 2 // bytes — 12, see header

/**
 * `callHelper` — push + enter, fused (§06). In: r1 = packed record
 * (P_idx | K+1<<16), r2 = Q_idx. Never returns — tail-jumps into Q's
 * resident code or the translator trampoline.
 */
export function emitCallHelper(): number[]
{
    return [
        arm.movHi(ENTRY_JUMP_REG, CONTROL_SP_REG),          // MOV r3, r9 — low-mirror the control stack pointer
        arm.stmia(ENTRY_JUMP_REG, [ENTRY_IDX_REG]),         // STMIA r3!, {r1} — push the record
        arm.movHi(CONTROL_SP_REG, ENTRY_JUMP_REG),          // MOV r9, r3 — publish the advance
        arm.lslsImm(ENTRY_IDX_REG, ENTRY_OFFSET_REG, 3),    // LSLS r1, r2, #3 — slotAddr-relative = Q_idx * 8
        arm.addHi(ENTRY_IDX_REG, DISPATCH_BASE_REG),        // ADD r1, r1, r8 — r1 = slotAddr
        arm.ldr5(ENTRY_JUMP_REG, ENTRY_IDX_REG, 0),         // LDR r3, [r1, #0] — r3 = code_ptr
        arm.movsImm8(ENTRY_OFFSET_REG, 1),                  // MOVS r2, #1 — offset+1 = 1, hardwired
        arm.bx(ENTRY_JUMP_REG),                             // BX r3
    ]
}

/**
 * `returnHelper` — pop + enter, fused (§06). In: nothing — everything
 * needed is already on the control stack. Never returns — tail-jumps into
 * the caller's resident code, the translator trampoline, or (the sentinel
 * `proc_idx = -1` record `enter_program` bootstraps with, §09) straight
 * into the info block's landing address.
 *
 * `SXTH`, not `UXTH` (this session's fix): the popped record's `proc_idx`
 * half must sign-extend, or the sentinel `-1` resolves to `r8 + 524280`
 * instead of `r8 - 8`. Identical to `UXTH` for every real index (they only
 * diverge once bit 15 is set, i.e. ≥32768 procedures).
 */
export function emitReturnHelper(): number[]
{
    return [
        arm.movHi(ENTRY_JUMP_REG, CONTROL_SP_REG),          // MOV r3, r9 — low-mirror the control stack pointer
        arm.subsImm8(ENTRY_JUMP_REG, 4),                    // SUBS r3, r3, #4 — back up to the last-pushed record
        arm.ldr5(ENTRY_IDX_REG, ENTRY_JUMP_REG, 0),         // LDR r1, [r3, #0] — r1 = packed record
        arm.movHi(CONTROL_SP_REG, ENTRY_JUMP_REG),          // MOV r9, r3 — publish the retreat
        arm.lsrsImm(ENTRY_OFFSET_REG, ENTRY_IDX_REG, 16),   // LSRS r2, r1, #16 — r2 = offset+1
        arm.sxth(ENTRY_IDX_REG, ENTRY_IDX_REG),             // SXTH r1, r1 — r1 = caller_idx, sign-extended
        arm.lslsImm(ENTRY_IDX_REG, ENTRY_IDX_REG, 3),       // LSLS r1, r1, #3 — slotAddr-relative = caller_idx * 8
        arm.addHi(ENTRY_IDX_REG, DISPATCH_BASE_REG),        // ADD r1, r1, r8 — r1 = slotAddr
        arm.ldr5(ENTRY_JUMP_REG, ENTRY_IDX_REG, 0),         // LDR r3, [r1, #0] — r3 = code_ptr
        arm.bx(ENTRY_JUMP_REG),                             // BX r3
    ]
}

/** Pack a control-stack/bootstrap record — `proc_idx` truncates to u16 (so
 *  `-1` becomes the `0xffff` sentinel `returnHelper`'s `SXTH` fix resolves
 *  to "one slot behind the dispatch table base"), `offsetPlus1` occupies the
 *  high half unchanged. */
export function packRecord(procIdx: number, offsetPlus1: number): number
{
    return ((procIdx & 0xffff) | (offsetPlus1 << 16)) >>> 0
}
