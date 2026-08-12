/**
 * @ppl/jit-armv6m-prototype — block structure (docs/jit-armv6m.md §7.1/§7.2)
 *
 * The "stack-like structure simply for parsing to keep track of block
 * structure" — an explicit array, not JS recursion (unlike validate.ts's
 * `walk`), because that's what a real single-pass translator emitting one
 * flat native-code stream needs: it marches a `pc` cursor forward exactly
 * once per bytecode instruction, and `BR_TABLE`/`LOOP` only ever push/pop
 * this stack, never recurse into a nested call. Mirrors vm.ts's own
 * `BlockFrame` union in *shape* (case / loopCond / loopBody) — repurposed
 * here for backpatch bookkeeping instead of runtime dispatch.
 *
 * Confirms docs/jit-armv6m.md §16 item 1's open question, for the branch-
 * target half of it: every branch this translator emits resolves its
 * target by the time its *own* enclosing `BLOCK_END`/back-edge is reached
 * — a `LOOP` back-edge target is already known the instant `LOOP` opens
 * (§7.2's own structure — the condition block starts right there), and a
 * `BR_TABLE` case's "skip to next case"/"skip to end" targets resolve the
 * moment that case (or the whole construct) closes. No separate pass over
 * the whole procedure is needed for *this* — only for what emit.ts's own
 * header flags as still open (out-of-range branches, `BR_TABLE N>2` jump
 * tables), neither implemented here.
 *
 * Scope: `BR_TABLE` is only implemented for `N ∈ {1, 2}` — isa-core.md
 * §7.1's `if`/`if-else` forms, which is everything the current test corpus
 * (leb128_len plus the four core-testsuite algorithms — none use `switch`)
 * needs; `N > 2` throws.
 */

import { Emitter } from "./emit"
import { Window, restoreWindow } from "./window"
import { AccState, Shape, SCRATCH_REG } from "./accstate"
import * as arm from "./armv6"
import type { BinaryOpcode } from "@ppl/machine"

type Frame =
    | { kind: "case"; entryTos: number; remaining: number; nextCaseFixup: number | null; endFixups: number[] }
    | { kind: "loopCond"; entryTos: number; loopStart: number }
    | { kind: "loopBody"; entryTos: number; loopStart: number; exitFixup: number }

export class BlockStack
{
    private readonly frames: Frame[] = []

    get isEmpty(): boolean { return this.frames.length === 0 }

    /** The open block immediately enclosing the current position, if any —
     *  translateProc.ts uses this to decide whether a still-pending fused
     *  comparison belongs to the `BLOCK_END` about to run (only true for
     *  `loopCond`) or is a dangling bug. */
    topKind(): Frame["kind"] | null
    {
        return this.frames.length === 0 ? null : this.frames[this.frames.length - 1]!.kind
    }

    /** `isa-core.md §7.1`: `acc < N` executes `case[acc]`. Only the
     *  branch-fusion shape is implemented — `condition` is the *true*
     *  Thumb condition of whatever comparison immediately preceded this
     *  `BR_TABLE` (blocks.ts's own caller, translateProc.ts, is the one
     *  that knows that; this function only knows it's a condition). */
    openBrTable(e: Emitter, window: Window, n: number, condition: arm.Condition): void
    {
        if(n !== 1 && n !== 2)
            throw new Error(`blocks: BR_TABLE ${n} not implemented — only if/if-else (N∈{1,2}, isa-core.md §7.1) are supported so far`)

        const frame: Frame = { kind: "case", entryTos: window.tos, remaining: n, nextCaseFixup: null, endFixups: [] }
        const site = e.emit(arm.condBranch(condition, 0))
        if(n === 1) frame.endFixups.push(site) // no case[1] to skip to — the branch's target IS "end of construct"
        else frame.nextCaseFixup = site // target is "start of case[1]", resolved the moment case[0] closes below
        this.frames.push(frame)
    }

    openLoop(e: Emitter, window: Window): void
    {
        this.frames.push({ kind: "loopCond", entryTos: window.tos, loopStart: e.pc })
    }

    /** `isa-core.md §7.2`/§8.1: any TOS surplus above the block's own
     *  entry depth is dropped here, restoring r4-r7 to what the *target*
     *  depth's window mapping expects (window.ts's `restoreWindow`) before
     *  any of this function's own branch bookkeeping runs. */
    closeBlockEnd(e: Emitter, window: Window, loopExitCondition: arm.Condition | null): void
    {
        const top = this.frames[this.frames.length - 1]
        if(!top) throw new Error(`blocks: BLOCK_END with no open block`)

        if(top.kind === "case")
        {
            restoreWindow(e, window, top.entryTos)
            top.remaining -= 1
            if(top.remaining > 0)
            {
                // Non-last case: falling off the end of its own code must
                // not continue into the next sibling case's code, which
                // immediately follows in memory — skip to the end of the
                // whole construct instead. This instruction is itself
                // part of *this* case's own tail, so `nextCaseFixup`
                // (guarding entry to the *next* case) must resolve to
                // whatever comes after it, not before — patching in the
                // opposite order patched a branch to jump straight into
                // this skip instruction instead of past it.
                top.endFixups.push(e.emit(arm.b(0)))
            }
            if(top.nextCaseFixup !== null) { e.patchBranch(top.nextCaseFixup, e.pc); top.nextCaseFixup = null }
            if(top.remaining > 0)
                return // stay on this frame — now translating the next case
            for(const site of top.endFixups) e.patchBranch(site, e.pc)
            this.frames.pop()
            return
        }

        if(top.kind === "loopCond")
        {
            restoreWindow(e, window, top.entryTos)
            if(loopExitCondition === null) throw new Error(`blocks: LOOP condition block closed with no fused comparison to branch on`)
            const exitFixup = e.emit(arm.condBranch(loopExitCondition, 0))
            this.frames.pop()
            this.frames.push({ kind: "loopBody", entryTos: top.entryTos, loopStart: top.loopStart, exitFixup })
            return
        }

        // loopBody: unconditional back-edge, then the earlier exit branch
        // resolves to right after it — both targets were knowable without
        // ever looking past this point (see this file's header).
        restoreWindow(e, window, top.entryTos)
        e.emit(arm.b(top.loopStart - (e.pc + 4)))
        e.patchBranch(top.exitFixup, e.pc)
        this.frames.pop()
    }
}

// ── Comparison → branch fusion (§10.1's "zero-destination" axis) ───────────

const DIRECT_CONDITION: Partial<Record<BinaryOpcode, arm.Condition>> = {
    EQ: arm.Condition.EQ, NE: arm.Condition.NE,
    LT_S: arm.Condition.LT, LE_S: arm.Condition.LE, GT_S: arm.Condition.GT, GE_S: arm.Condition.GE,
    LT_U: arm.Condition.LO, LE_U: arm.Condition.LS, GT_U: arm.Condition.HI, GE_U: arm.Condition.HS,
}

export function isComparisonOp(op: BinaryOpcode): boolean
{
    return op in DIRECT_CONDITION
}

/**
 * Emit the `CMP` for a comparison whose *only* consumer is the following
 * `BR_TABLE`/`LOOP`-condition `BLOCK_END` — never materializes a 0/1
 * result (that's the whole point of this fusion axis). Returns the Thumb
 * condition that's true exactly when the comparison itself is true; the
 * caller (translateProc.ts) hands that to `openBrTable`/`closeBlockEnd`
 * directly or inverts it first, depending which side needs "true".
 *
 * Doesn't implement §10.1's immediate-side mirror-table optimization
 * (swapping `k < rN` to `rN > k` so an immediate acc can fold as `CMP`'s
 * `Rn`) — this corpus's comparisons always have a register-shaped acc in
 * practice (a just-loaded variable), so the rare imm-on-the-left case just
 * flushes to `ACC_REG` first via `shapeToReg`-equivalent inline logic,
 * always correct, occasionally one instruction longer than optimal.
 */
export function emitComparison(e: Emitter, accState: AccState, op: BinaryOpcode, operand: Shape | undefined): arm.Condition
{
    const condition = DIRECT_CONDITION[op]
    if(condition === undefined) throw new Error(`blocks: ${op} is not a comparison`)

    let left = accState.peek()
    if(left.kind === "imm")
    {
        arm.synthesizeImm32(3, left.value).forEach(w => e.emit(w)) // ACC_REG=3; kept local to avoid a cross-import just for one constant
        left = { kind: "reg", reg: 3 }
    }

    if(operand === undefined)
        throw new Error(`blocks: PEEK_PEEK comparison fusion is not implemented (not exercised by this corpus)`)

    if(operand.kind === "reg")
    {
        e.emit(arm.cmpReg(left.reg, operand.reg))
    }
    else if(arm.fitsImm8(operand.value))
    {
        e.emit(arm.cmpImm8(left.reg, operand.value))
    }
    else
    {
        arm.synthesizeImm32(SCRATCH_REG, operand.value).forEach(w => e.emit(w))
        e.emit(arm.cmpReg(left.reg, SCRATCH_REG))
    }
    return condition
}
