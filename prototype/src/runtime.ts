/**
 * @ppl/jit-armv6m-prototype — the per-procedure prologue stub
 * (docs/jit-armv6m-dispatch-handoff.html §06), TS-emitted via armv6.ts
 * like everything else this prototype generates.
 *
 * The dividing line with qemu/runtime.S (`callHelper`/`returnHelper`/the
 * translator-entry trampoline) isn't "which one is simpler to encode" —
 * `callHelper`/`returnHelper` are self-contained too, no absolute
 * addresses, and could just as well be TS-emitted bytes. It's whether the
 * routine ever gets *copied* to a dynamic runtime address: the stub is
 * copied into the arena ahead of every compiled procedure's own body, so
 * it has to stay data the mock translator can memcpy, not code reached by
 * name — everything reached by a fixed address for the program's whole
 * lifetime belongs in qemu/runtime.S instead, hand-written and built by
 * the normal toolchain like ../../src/vectors.S's own reset handler.
 */

import * as arm from "./armv6"
import { ENTRY_IDX_REG, ENTRY_OFFSET_REG, ENTRY_JUMP_REG, LRU_TICK_REG } from "./registers"

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

/** Pack a control-stack/bootstrap record — `proc_idx` truncates to u16 (so
 *  `-1` becomes the `0xffff` sentinel `returnHelper`'s `SXTH` fix resolves
 *  to "one slot behind the dispatch table base"), `offsetPlus1` occupies the
 *  high half unchanged. */
export function packRecord(procIdx: number, offsetPlus1: number): number
{
    return ((procIdx & 0xffff) | (offsetPlus1 << 16)) >>> 0
}
