/**
 * @ppl/jit-armv6m-prototype — unary op codegen (docs/design.md §10, §16 item 8)
 *
 * `NEG`/`NOT` are single native instructions (Thumb-1's `RSB`-with-
 * implicit-`#0` idiom / `MVN`, §10.1's "destination-only, no own-value
 * read" row — same shape as binops.ts's own classification table, just
 * with no combo to speak of). `CLZ`/`REVBITS` have no ARMv6-M native form
 * at all (`CLZ` is ARMv5T/Thumb-2, `RBIT` is ARMv7-M+ — design.md §10's own
 * note), so both go through a small, per-procedure-shared software
 * routine reached by a local `BL` — exactly `blocks.ts`'s
 * `emitBrTableHelper` precedent (`BR_TABLE N>2`'s own "one shared routine,
 * every site in this procedure that needs it" pattern), not the real,
 * whole-program-linked, flash-resident static helper vector docs/design.md
 * §11 envisions for the native target (reserved slots 4+ there) — a
 * prototype-appropriate simplification, not the final shape; the native
 * port should reach these through that vector instead, once it exists.
 *
 * Both helpers only ever touch `r0`-`r2` — never a window register — so
 * reaching either one needs no window save/restore around the call,
 * unlike a real `CALL`. They *do* clobber `lr` like any other nested
 * `BL`, though — `translateProc.ts`'s `needsLRSave` has to know that.
 */

import { Emitter } from "./emit"
import * as arm from "./armv6"
import { ACC_REG, SCRATCH_REG } from "./registers"
import type { UnaryOpcode } from "@ppl/machine"

const LR = 14

/** Placeholder `BL` sites collected per procedure, one list per software
 *  helper — translateProc.ts patches each list once its own copy of the
 *  relevant helper is emitted (mirroring `brTableHelperSites`). */
export interface UnaryHelperSites
{
    clz: number[]
    revbits: number[]
}

export function newUnaryHelperSites(): UnaryHelperSites
{
    return { clz: [], revbits: [] }
}

/**
 * Emit one unary op. `operand` must already be materialized into
 * `ACC_REG` (the caller's job — unlike binops.ts's fold machinery, a
 * unary op's native encoding never takes an immediate form at all, so
 * there's nothing to fold, only something to flush first). `dest` is
 * `ACC_REG` or a destination-fold target.
 */
export function emitUnary(e: Emitter, op: UnaryOpcode, dest: number, helperSites: UnaryHelperSites): void
{
    if(op === "NEG") { e.emit(arm.negs(dest, ACC_REG)); return }
    if(op === "NOT") { e.emit(arm.mvns(dest, ACC_REG)); return }

    const site = e.placeholderBL()
    if(op === "CLZ") helperSites.clz.push(site)
    else helperSites.revbits.push(site)
    if(dest !== ACC_REG) e.emit(arm.movHi(dest, ACC_REG))
}

/**
 * Count leading zeros (0..32) — a straight-line shift-and-test loop, one
 * bit per iteration: Thumb-1's shift-immediate instructions set the carry
 * flag from the bit shifted out, so `LSLS`+`BCS` directly tests the
 * current MSB without a separate mask/compare. `r0` in/out, `r1` the
 * running count.
 */
export function emitClzHelper(e: Emitter): number
{
    const COUNT_REG = 1
    const start = e.pc
    e.emit(arm.movsImm8(COUNT_REG, 0))
    e.emit(arm.cmpImm8(ACC_REG, 0))
    const zeroSite = e.placeholderCondBranch(arm.Condition.EQ)
    const loopStart = e.pc
    e.emit(arm.lslsImm(ACC_REG, ACC_REG, 1)) // carry = the bit shifted out, i.e. the old bit 31
    const doneSite = e.placeholderCondBranch(arm.Condition.HS) // carry set == that bit was 1
    e.emit(arm.addsImm8(COUNT_REG, 1))
    e.emit(arm.b(loopStart - (e.pc + 4)))
    e.patchBranch(doneSite, e.pc)
    e.emit(arm.movHi(ACC_REG, COUNT_REG))
    e.emit(arm.bx(LR))
    e.patchBranch(zeroSite, e.pc)
    e.emit(arm.movsImm8(ACC_REG, 32))
    e.emit(arm.bx(LR))
    return start
}

/**
 * Reverse bit order (32-bit) — one bit per iteration: shift the source
 * right (carry = its own bit 0), then fold that bit into the result's own
 * bottom via `ADCS Rd,Rd` (`Rd = Rd + Rd + carry`, i.e. "double the
 * result and add the carry in") — 32 iterations place the source's bit 0
 * at the result's bit 31 and so on down to bit 31 landing at bit 0,
 * exactly the reversal. `r0` in/out, `r1` the source (consumed), `r2`
 * (`SCRATCH_REG`) the remaining-bit counter.
 */
export function emitRevbitsHelper(e: Emitter): number
{
    const SRC_REG = 1
    const start = e.pc
    e.emit(arm.movHi(SRC_REG, ACC_REG))
    e.emit(arm.movsImm8(ACC_REG, 0))
    e.emit(arm.movsImm8(SCRATCH_REG, 32))
    const loopStart = e.pc
    e.emit(arm.lsrsImm(SRC_REG, SRC_REG, 1)) // carry = the bit shifted out, i.e. the old bit 0
    e.emit(arm.adcs(ACC_REG, ACC_REG))
    e.emit(arm.subsImm8(SCRATCH_REG, 1))
    const doneSite = e.placeholderCondBranch(arm.Condition.EQ)
    e.emit(arm.b(loopStart - (e.pc + 4)))
    e.patchBranch(doneSite, e.pc)
    e.emit(arm.bx(LR))
    return start
}
