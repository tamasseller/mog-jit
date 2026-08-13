/**
 * @ppl/jit-armv6m-prototype — per-procedure translation (docs/jit-armv6m.md §10)
 *
 * The single instruction-at-a-time sink the whole prototype is organized
 * around: one forward `pc` walk over `proc.body`, dispatching each
 * instruction to whichever piece of state it belongs to — window.ts's
 * `tos` counter, accstate.ts's CLEAN/PENDING slot, or blocks.ts's explicit
 * block stack — never more than a one-token lookahead (the STORE-fold
 * peek), matching §10.1's own "no lookahead past that" argument.
 *
 * Scope (see the package README/this session's notes): `CALL` is not
 * implemented — §6's calling-convention shuffle is this design's own
 * flagged-as-unproven piece (§16 item 1), deliberately isolated as its own
 * follow-up rather than rushed in alongside everything else. `RETURN`
 * accordingly isn't the doc's re-enterable `dispatch_return` mechanism
 * (§7) either — with no eviction and no `CALL`, a procedure can just be a
 * plain native subroutine (`BX LR`); the doc's own dispatch-table return
 * path only becomes necessary once `CALL` exists. `BR_TABLE` is
 * implemented for `N ∈ {1, 2}` (if/if-else) only — no `N > 2` jump tables.
 * Unary ops (`NEG`/`NOT`/`CLZ`/`REVBITS`) and `EXT` throw — unexercised by
 * the current test corpus, not yet implemented.
 */

import { Emitter } from "./emit"
import { Window, physReg } from "./window"
import { AccState, emitBinary } from "./accstate"
import { Shape } from "./shape"
import { ACC_REG } from "./registers"
import { BlockStack, emitComparison, isComparisonOp, testAccNonzero } from "./blocks"
import * as arm from "./armv6"
import type { RtlProc, RtlInstr, ComboName } from "@ppl/machine"

const LR = 14

/** Slot k's window register a peek at `body[pc+1]` resolves to, if that
 *  next instruction is a `STORE` — the one-token destination-fold trigger
 *  every producer/consumer below checks before falling back to `ACC_REG`. */
function peekStoreFold(body: readonly RtlInstr[], pc: number): number | null
{
    const next = body[pc + 1]
    if(next && next.op === "STORE") return physReg(next.target)
    return null
}

/** Rotation-eviction guard (window.ts's/accstate.ts's own doc comments):
 *  refuse rather than silently miscompile if a still-open pending/clean
 *  value would be evicted by the push about to happen. */
function pushAcc(e: Emitter, window: Window, accState: AccState): void
{
    if(window.pushEvicts)
    {
        const dep = accState.dependsOnReg()
        if(dep !== null && dep === physReg(window.evictedByPush))
            throw new Error(`translateProc: rotation-eviction fallback not implemented (docs/jit-armv6m.md §10.1, "essentially never fires" — this corpus apparently hit it)`)
    }
    window.emitSpillIfNeeded(e)
    accState.flush(e, physReg(window.tos))
    window.push()
}

export function translateProc(proc: RtlProc, localPeak: number): Uint16Array
{
    const e = new Emitter()
    const window = new Window(proc.argCount)
    const accState = new AccState()
    const blocks = new BlockStack()
    const body = proc.body
    const spillBytes = localPeak * 4

    e.emit(arm.decrSp(spillBytes)) // prologue — reserve this procedure's own spill area

    // §6 callee-side prologue: the last argument (if any) arrives in acc —
    // ACC_REG, by this prototype's own native ABI choice, matching the
    // doc's own convention exactly — and must land at phys(0) before
    // anything reads it via LOAD/STORE. Unconditional, not folded (§6's
    // "as a fold" optimization is not implemented — a documented, minor,
    // correctness-neutral gap).
    if(proc.argCount >= 1) accState.flush(e, physReg(0))

    let pendingComparisonCondition: arm.Condition | null = null
    let pc = 0

    function returnSequence(): void
    {
        e.emit(arm.incrSp(spillBytes))
        e.emit(arm.bx(LR))
    }

    while(pc < body.length)
    {
        const instr = body[pc]!

        if(instr.op === "CALL")
            throw new Error(`translateProc: CALL not implemented — §6's shuffle is its own follow-up milestone (docs/jit-armv6m.md §16 item 1)`)
        if(instr.op === "EXT")
            throw new Error(`translateProc: EXT not implemented`)
        if(instr.op === "NEG" || instr.op === "NOT" || instr.op === "CLZ" || instr.op === "REVBITS")
            throw new Error(`translateProc: unary op ${instr.op} not implemented (not exercised by the current test corpus)`)

        if(instr.op === "BLOCK_END")
        {
            const topKind = blocks.topKind()
            let loopExitCond: arm.Condition | null = null
            if(topKind === "loopCond")
            {
                // isa-core.md §7.2's own leniency (blocks.ts's `testAccNonzero`
                // doc comment) — fall back to an explicit `CMP #0` when
                // nothing was fused, rather than requiring the preceding
                // instruction to have been a comparison.
                const trueCondition = pendingComparisonCondition ?? testAccNonzero(e, accState)
                pendingComparisonCondition = null
                loopExitCond = arm.inverse(trueCondition)
            }
            else if(pendingComparisonCondition !== null)
            {
                throw new Error(`translateProc: comparison fused into nothing (dangling condition at BLOCK_END, pc=${pc})`)
            }
            blocks.closeBlockEnd(e, window, loopExitCond)
            pc++
            continue
        }

        if(instr.op === "LOOP") { blocks.openLoop(e, window); pc++; continue }

        if(instr.op === "BR_TABLE")
        {
            const trueCondition = pendingComparisonCondition ?? testAccNonzero(e, accState)
            blocks.openBrTable(e, window, instr.imm, trueCondition)
            pendingComparisonCondition = null
            pc++
            continue
        }

        if(instr.op === "RETURN")
        {
            accState.flush(e, 0) // r0 — this prototype's own return-value register
            returnSequence()
            pc++
            continue
        }

        if(instr.op === "TRAP")
        {
            // No dispatch/report machinery exists yet (that's CALL's own
            // follow-up territory) — sentinel-encode the trap so the QEMU
            // trampoline (qemu/run.c) can tell it apart from a normal
            // return: high bit set, low bits the trap code.
            arm.synthesizeImm32(0, 0x80000000 | instr.imm).forEach(w => e.emit(w))
            returnSequence()
            pc++
            continue
        }

        if(instr.op === "PUSH") { pushAcc(e, window, accState); pc++; continue }

        if(instr.op === "POP")
        {
            const poppedReg = physReg(window.tos - 1)
            e.emit(arm.movHi(ACC_REG, poppedReg)) // materialize now — see accstate.ts's header for why a bare POP can't safely stay PENDING
            accState.setClean(ACC_REG)
            window.emitFillIfNeeded(e) // must run after the read above — same register
            window.pop()
            pc++
            continue
        }

        if(instr.op === "LOAD")
        {
            const foldTarget = peekStoreFold(body, pc)
            accState.producer({ kind: "reg", reg: physReg(instr.target) })
            if(foldTarget !== null) { accState.flush(e, foldTarget); pc += 2; continue }
            pc++
            continue
        }

        if(instr.op === "STORE")
        {
            accState.flush(e, physReg(instr.target))
            pc++
            continue
        }

        if(instr.op === "CONST")
        {
            const foldTarget = peekStoreFold(body, pc)
            const target = foldTarget ?? ACC_REG
            if(arm.fitsImm8(instr.imm) && foldTarget === null)
            {
                accState.producer({ kind: "imm", value: instr.imm }) // stay pending — a later consumer may fold it
                pc++
                continue
            }
            arm.synthesizeImm32(target, instr.imm).forEach(w => e.emit(w))
            accState.setClean(target)
            pc += foldTarget !== null ? 2 : 1
            continue
        }

        if("combo" in instr)
        {
            const combo: ComboName = instr.combo
            let operand: Shape | undefined
            let popAfter = false

            if(instr.combo === "REG_ACC" || instr.combo === "REG_REG") operand = { kind: "reg", reg: physReg(instr.target) }
            else if(instr.combo === "IMM_ACC") operand = { kind: "imm", value: instr.imm }
            else if(instr.combo === "POP_ACC") { operand = { kind: "reg", reg: physReg(window.tos - 1) }; popAfter = true }
            else operand = undefined // PEEK_PEEK

            if(isComparisonOp(instr.op))
            {
                const trueCondition = emitComparison(e, accState, instr.op, operand)
                if(popAfter) { window.emitFillIfNeeded(e); window.pop() }
                pendingComparisonCondition = trueCondition
                pc++
                continue
            }

            const clobbersAcc = combo === "REG_REG" || combo === "PEEK_PEEK"
            let dest: number
            let consumedStore = false
            if(instr.combo === "REG_REG") dest = physReg(instr.target)
            else if(instr.combo === "PEEK_PEEK") dest = physReg(window.tos - 1)
            else
            {
                const foldTarget = peekStoreFold(body, pc)
                dest = foldTarget ?? ACC_REG
                consumedStore = foldTarget !== null
            }

            emitBinary(e, accState, instr.op, combo, operand, dest, clobbersAcc)
            if(popAfter) { window.emitFillIfNeeded(e); window.pop() }
            pc += consumedStore ? 2 : 1
            continue
        }

        throw new Error(`translateProc: unhandled instruction ${JSON.stringify(instr)} at pc ${pc}`)
    }

    if(!blocks.isEmpty) throw new Error(`translateProc: procedure body ended with an open block`)
    return e.toUint16Array()
}
