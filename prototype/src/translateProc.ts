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
 * Scope (see the package README/this session's notes): §6's calling-
 * convention shuffle (`CALL`, below) is this design's own flagged-as-
 * unproven piece (§16 item 1) — implemented here using the whole-program
 * up-front layout this prototype already has (no dispatch table, no
 * eviction, so a plain `BL` stands in for §9's real `BLX`-through-a-table;
 * see armv6.ts's own note on `bl`). `RETURN` accordingly isn't the doc's
 * re-enterable `dispatch_return` mechanism (§7) either — with no eviction,
 * a procedure can just be a plain native subroutine (saving/restoring its
 * own `lr` only if its own body makes a nested `CALL`, exactly like any
 * ordinary calling convention); the doc's own dispatch-table return path
 * only becomes necessary once eviction does. `BR_TABLE N > 2` compiles to
 * a shared per-procedure jump-table helper (blocks.ts's
 * `openBrTableJump`/`emitBrTableHelper`) instead of the branch-fusion path
 * `N ∈ {1, 2}` uses. No upper bound on a procedure's own `argCount` or any
 * `CALL`'s stack-passed arg count either — `LOAD`/`STORE`/register-mode
 * operands all fall back to real stack addressing (`window.ts`'s
 * `spillOffset`) the moment their target slot has fallen out of the
 * 4-register window, exactly as `PUSH`/`POP` already do at the window
 * boundary (§5). Unary ops (`NEG`/`NOT`/`CLZ`/`REVBITS`) and `EXT` still
 * throw — unexercised by the current test corpus, not yet implemented.
 */

import { Emitter } from "./emit"
import { Window, physReg, inWindow, spillOffset, discardWindow, spillForCall, fillCalleeArgs, reloadAfterCall } from "./window"
import { AccState, emitBinary } from "./accstate"
import { Shape } from "./shape"
import { ACC_REG, SCRATCH_REG } from "./registers"
import { BlockStack, emitComparison, isComparisonOp, testAccNonzero, emitBrTableHelper } from "./blocks"
import * as arm from "./armv6"
import type { RtlProc, RtlInstr, ComboName } from "@ppl/machine"

const LR = 14

/** One not-yet-resolved `CALL` site — `siteOffset` is local to this
 *  procedure's own emitted code; program.ts is the only thing that can
 *  resolve `calleeIndex` to a final target, once every procedure's own
 *  layout is known (this file, and its own `Emitter`, never see more than
 *  one procedure at a time — see armv6.ts's `bl` header). */
export interface CallSite
{
    readonly siteOffset: number
    readonly calleeIndex: number
}

/** Slot k's window register a peek at `body[pc+1]` resolves to, if that
 *  next instruction is a `STORE` targeting a currently in-window slot —
 *  the one-token destination-fold trigger every producer/consumer below
 *  checks before falling back to `ACC_REG`. Out-of-window `STORE` targets
 *  don't fold: there's no register to fold into (the target lives on the
 *  real stack, `spillOffset` below), so the fold is skipped and the
 *  ordinary `STORE` case (which does know how to reach it) handles it on
 *  the next iteration instead. */
function peekStoreFold(body: readonly RtlInstr[], pc: number, tos: number): number | null
{
    const next = body[pc + 1]
    if(next && next.op === "STORE" && inWindow(tos, next.target)) return physReg(next.target)
    return null
}

export interface TranslatedProc
{
    readonly code: Uint16Array
    readonly callSites: readonly CallSite[]
}

/** `calleeArgCounts` is every procedure's own `argCount`, indexed by
 *  procedure-table index (program.ts's `program.procedures.map(p =>
 *  p.argCount)`) — enough for `CALL` (below) to know how many of its own
 *  currently-resident args are stack-passed (`argCount - 1`, isa-core.md
 *  §4.6) without needing the whole `RtlProgram` type. Defaults to `[]`
 *  since most of the existing test corpus has no `CALL` at all and never
 *  indexes it. */
export function translateProc(proc: RtlProc, calleeArgCounts: readonly number[] = []): TranslatedProc
{
    const e = new Emitter()
    const window = new Window(proc.argCount)
    const accState = new AccState()
    const blocks = new BlockStack()
    const callSites: CallSite[] = []
    const body = proc.body

    // A `CALL` anywhere in this body clobbers `lr` (`BL` sets it); this
    // procedure's own incoming `lr` — its return address into *its own*
    // caller — only survives a nested call if it's saved first. Known
    // upfront from a plain body scan — not a violation of "no
    // cross-instruction analysis" so much as the one piece of whole-body
    // metadata the prologue's own *shape* needs before it can be emitted
    // at all. `BR_TABLE N>2` clobbers `lr` exactly the same way — its own
    // dispatch is a local `BL` to the shared jump-table helper
    // (blocks.ts's `openBrTableJump`) — so it needs the identical
    // save/restore, not a CALL-specific check.
    const savesLR = body.some(i => i.op === "CALL" || (i.op === "BR_TABLE" && i.imm > 2))

    // prologue — save `lr` only if this body needs to. No fixed spill-area
    // reservation at all (window.ts's own header): every spill/fill is a
    // real sp-adjusting PUSH/POP, so sp naturally tracks actual depth with
    // nothing reserved up front.
    if(savesLR) e.emit(arm.pushWithLr([]))

    // §6 callee-side prologue: the last argument (if any) arrives in acc —
    // ACC_REG, by this prototype's own native ABI choice, matching the
    // doc's own convention exactly — and must land at phys(argCount-1),
    // its own frame-relative slot, before anything reads it via
    // LOAD/STORE. Unconditional, not folded (§6's "as a fold" optimization
    // is not implemented — a documented, minor, correctness-neutral gap).
    if(proc.argCount >= 1) accState.flush(e, physReg(proc.argCount - 1))

    let pendingComparisonCondition: arm.Condition | null = null
    let pc = 0

    function returnSequence(): void
    {
        // Unwind whatever this body spilled — nothing downstream reads
        // r4-r7 again, so there's nothing to reload for (window.ts's
        // `discardWindow`), only sp to rebalance before lr comes back.
        discardWindow(e, window)
        if(savesLR) e.emit(arm.popWithPc([])) // pops the saved lr straight into pc — this *is* the return
        else e.emit(arm.bx(LR))
    }

    while(pc < body.length)
    {
        const instr = body[pc]!

        switch(instr.op)
        {
            case "CALL":
            {
                const calleeArgCount = calleeArgCounts[instr.calleeIndex]
                if(calleeArgCount === undefined)
                    throw new Error(`translateProc: CALL ${instr.calleeIndex}: no such procedure`)
                const stackArgs = Math.max(calleeArgCount - 1, 0) // isa-core.md §4.6

                // acc is unconditionally clobbered by CALL — the callee's
                // own last argument (if any) going in, its return value
                // coming out — so whatever's pending here just needs
                // materializing, not any special-casing per argCount.
                accState.flush(e, ACC_REG)

                // §6's shuffle: spill everything currently resident (the
                // caller's own leftover locals, if any, individually —
                // natural order; the stack-passed args batched, since
                // they're about to be popped straight back out — see
                // window.ts's own header), then fill the callee's own
                // canonical phase-0 window from that.
                spillForCall(e, window, stackArgs)
                fillCalleeArgs(e, stackArgs)

                callSites.push({ siteOffset: e.placeholderBL(), calleeIndex: instr.calleeIndex })

                // The callee has freely clobbered r4-r7 for its own,
                // unrelated window — reload the caller's own, now that
                // `stackArgs` slots have been consumed by the call.
                reloadAfterCall(e, window, window.tos - stackArgs)

                // The return value is now in acc — a fresh producer, same
                // as any other (LOAD/POP/CONST), so a following STORE
                // still folds.
                accState.producer({ kind: "reg", reg: ACC_REG })
                pc++
                continue
            }

            case "EXT":
                throw new Error(`translateProc: EXT not implemented`)

            case "NEG": case "NOT": case "CLZ": case "REVBITS":
                throw new Error(`translateProc: unary op ${instr.op} not implemented (not exercised by the current test corpus)`)

            case "BLOCK_END":
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
                blocks.closeBlockEnd(e, window, accState, loopExitCond)
                pc++
                continue
            }

            case "LOOP":
                blocks.openLoop(e, window)
                pc++
                continue

            case "BR_TABLE":
            {
                // N ≤ 2 (if/if-else): a boolean-shaped acc, branch-fusable
                // (§10.1) against whatever comparison (if any) immediately
                // preceded this. N > 2: acc is a genuine multi-way selector
                // — its actual value is what's needed, not a condition, so
                // there's nothing to fuse (a real switch selector is never
                // a comparison's own 0/1 result) and no `testAccNonzero`
                // `CMP #0` to pay for either.
                if(instr.imm > 2) blocks.openBrTableJump(e, window, instr.imm, accState)
                else
                {
                    const trueCondition = pendingComparisonCondition ?? testAccNonzero(e, accState)
                    blocks.openBrTable(e, window, instr.imm, trueCondition)
                }
                pendingComparisonCondition = null
                pc++
                continue
            }

            case "RETURN":
                accState.flush(e, ACC_REG) // isa-core.md §7: the return value is whatever's in acc
                returnSequence()
                pc++
                continue

            case "TRAP":
                // §12's real Report/error model isn't implemented — sentinel-
                // encode the trap so the QEMU trampoline (test/qemu-run.ts)
                // can tell it apart from a normal return: high bit set, low
                // bits the trap code. Lands in ACC_REG for the same reason
                // RETURN's own value does — it's what the harness reads.
                arm.synthesizeImm32(ACC_REG, 0x80000000 | instr.imm).forEach(w => e.emit(w))
                returnSequence()
                pc++
                continue

            case "PUSH":
                window.pushValue(e, accState)
                pc++
                continue

            case "POP":
                e.emit(arm.movHi(ACC_REG, window.topReg)) // materialize now — see accstate.ts's header for why a bare POP can't safely stay PENDING
                accState.setClean(ACC_REG)
                window.finishPop(e) // must run after the read above — same register
                pc++
                continue

            case "LOAD":
            {
                // Out-of-window: `physReg(instr.target)` would name some
                // *other*, currently-resident slot's register — this
                // target lives only on the real stack (window.ts's
                // `spillOffset`). No fold attempted here (matches §5's own
                // "leaves cheap dead-reload elimination on the table"
                // trade) — always one `LDR`, straight into `acc`.
                if(!inWindow(window.tos, instr.target))
                {
                    e.emit(arm.ldrSp(ACC_REG, spillOffset(window.tos, instr.target)))
                    accState.setClean(ACC_REG)
                    pc++
                    continue
                }
                const foldTarget = peekStoreFold(body, pc, window.tos)
                accState.producer({ kind: "reg", reg: physReg(instr.target) })
                if(foldTarget !== null) { accState.flush(e, foldTarget); pc += 2; continue }
                pc++
                continue
            }

            case "STORE":
                // Symmetric with LOAD above: materialize into acc first
                // (same as the in-window path would eventually need
                // anyway), then one `STR` to the real stack instead of a
                // register write.
                if(!inWindow(window.tos, instr.target))
                {
                    accState.flush(e, ACC_REG)
                    e.emit(arm.strSp(ACC_REG, spillOffset(window.tos, instr.target)))
                    pc++
                    continue
                }
                accState.flush(e, physReg(instr.target))
                pc++
                continue

            case "CONST":
            {
                const foldTarget = peekStoreFold(body, pc, window.tos)
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

            // Every remaining op (arithmetic + comparison) carries a
            // `combo` — the addressing-mode dimension isa-core.md §3/§4.1
            // layers onto every one of them uniformly, so they share one
            // dispatch path here instead of one `case` apiece.
            default:
            {
                const combo: ComboName = instr.combo
                let operand: Shape | undefined
                let popAfter = false

                if(instr.combo === "REG_ACC" || instr.combo === "REG_REG")
                {
                    // Same out-of-window concern as LOAD above: a register-
                    // mode operand naming a slot that's fallen out of the
                    // window has to come from the real stack, into scratch
                    // — never `physReg(instr.target)`, which would name
                    // whatever *else* currently lives there instead.
                    if(inWindow(window.tos, instr.target)) operand = { kind: "reg", reg: physReg(instr.target) }
                    else
                    {
                        e.emit(arm.ldrSp(SCRATCH_REG, spillOffset(window.tos, instr.target)))
                        operand = { kind: "reg", reg: SCRATCH_REG }
                    }
                }
                else if(instr.combo === "IMM_ACC") operand = { kind: "imm", value: instr.imm }
                else if(instr.combo === "POP_ACC") { operand = { kind: "reg", reg: window.topReg }; popAfter = true }
                else operand = undefined // PEEK_PEEK

                if(isComparisonOp(instr.op))
                {
                    const trueCondition = emitComparison(e, accState, instr.op, operand)
                    if(popAfter) window.finishPop(e)
                    pendingComparisonCondition = trueCondition
                    pc++
                    continue
                }

                const clobbersAcc = combo === "REG_REG" || combo === "PEEK_PEEK"
                let dest: number
                let consumedStore = false
                // REG_REG writes back in place (isa-core.md §4.1 mode 2) —
                // out-of-window, that write-back target is memory, not a
                // register: compute into SCRATCH_REG (already holding the
                // operand read above) and store it back explicitly.
                let storeBackOffset: number | null = null
                if(instr.combo === "REG_REG")
                {
                    if(inWindow(window.tos, instr.target)) dest = physReg(instr.target)
                    else { dest = SCRATCH_REG; storeBackOffset = spillOffset(window.tos, instr.target) }
                }
                else if(instr.combo === "PEEK_PEEK") dest = window.topReg
                else
                {
                    const foldTarget = peekStoreFold(body, pc, window.tos)
                    dest = foldTarget ?? ACC_REG
                    consumedStore = foldTarget !== null
                }

                emitBinary(e, accState, instr.op, combo, operand, dest, clobbersAcc)
                if(storeBackOffset !== null) e.emit(arm.strSp(dest, storeBackOffset))
                if(popAfter) window.finishPop(e)
                pc += consumedStore ? 2 : 1
                continue
            }
        }
    }

    if(!blocks.isEmpty) throw new Error(`translateProc: procedure body ended with an open block`)

    // BR_TABLE N>2's shared helper (blocks.ts's own header) — dead code
    // from a sequential-execution standpoint, reached only by the local
    // `BL`s `openBrTableJump` already placed; emitted once, here, only if
    // this body actually used it.
    if(blocks.brTableHelperSites.length > 0)
    {
        const helperOffset = emitBrTableHelper(e)
        for(const site of blocks.brTableHelperSites) e.patchBL(site, helperOffset)
    }

    return { code: e.toUint16Array(), callSites }
}
