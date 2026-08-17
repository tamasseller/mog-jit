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
 * header still flags as open: out-of-range conditional branches needing
 * the invert-and-long-branch idiom.
 *
 * `BR_TABLE N ∈ {1, 2}` (isa-core.md §7.1's `if`/`if-else` forms) fuses
 * against a preceding comparison, branch-fusion style (`openBrTable`). `N >
 * 2` (a genuine multi-way selector, no comparison to fuse against) instead
 * compiles to a shared per-procedure jump-table helper (`openBrTableJump`/
 * `emitBrTableHelper`) — a small validation of docs/jit-armv6m.md §11's own
 * "shared reserved routine, amortized over every call site" pattern
 * (there envisioned for `RETURN`'s `dispatch_return` and the `CLZ`/
 * `REVBITS` helpers), just reached by a local `BL` here since this
 * prototype has no cross-procedure dispatch table (§9) to hang a shared
 * *global* copy off of.
 */

import { Emitter } from "./emit"
import { Window, restoreWindow } from "./window"
import { AccState } from "./accstate"
import { Shape, materializeShape } from "./shape"
import { ACC_REG, SCRATCH_REG } from "./registers"
import * as arm from "./armv6"
import type { BinaryOpcode } from "@ppl/machine"

const LR = 14
/** Low-register scratch the jump-table helper copies `lr` into — `lr`
 *  itself can't be the base register of a register-offset `LDRH`
 *  (Thumb-1's 3-register addressing forms are low-register-only). Purely
 *  local to `emitBrTableHelper`'s own few instructions. */
const TABLE_PTR_REG = 3
/** The clamped case index, computed without ever touching `r0`/`ACC_REG`
 *  — `emitBrTableHelper`'s own doc comment explains why that matters.
 *  Purely local to the helper. */
const INDEX_REG = 1

type Frame =
    | {
        kind: "case"; entryTos: number; remaining: number
        nextCaseFixup: number | null
        /** Set only by `openBrTableJump` (`N > 2`): the jump table's own
         *  base offset (`lr`'s value once the dispatching `BL` runs), the
         *  still-unpatched slots for cases not yet opened (case 0's slot
         *  is always patched immediately, needing no fixup at all), and
         *  the one extra slot — index `N`, beyond every real case — for a
         *  genuinely out-of-range selector (isa-core.md's own "acc ≥ N":
         *  falls through with *no* case body run at all, same target as
         *  `endFixups` below, patched alongside them). */
        table: { base: number; fixups: number[]; endSlot: number } | null
        endFixups: number[]
      }
    | { kind: "loopCond"; entryTos: number; loopStart: number }
    | { kind: "loopBody"; entryTos: number; loopStart: number; exitFixup: number }

export class BlockStack
{
    private readonly frames: Frame[] = []

    /** Pending local `BL` sites (from `openBrTableJump`) waiting on
     *  `emitBrTableHelper`'s own offset — patched once, by translateProc.ts,
     *  after the procedure's main body is fully translated (the helper
     *  itself is emitted last, as dead code reached only by these `BL`s). */
    readonly brTableHelperSites: number[] = []

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

        const frame: Frame = { kind: "case", entryTos: window.tos, remaining: n, nextCaseFixup: null, table: null, endFixups: [] }
        const site = e.emit(arm.condBranch(condition, 0))
        if(n === 1) frame.endFixups.push(site) // no case[1] to skip to — the branch's target IS "end of construct"
        else frame.nextCaseFixup = site // target is "start of case[1]", resolved the moment case[0] closes below
        this.frames.push(frame)
    }

    /** `isa-core.md §7.1` for `N > 2`: a genuine multi-way selector, not a
     *  boolean — dispatches via a shared per-procedure helper
     *  (`emitBrTableHelper`) instead of a fused conditional branch. Emits,
     *  in order: materialize `acc`'s real value into `ACC_REG` (the
     *  helper needs the actual selector, not a condition — no
     *  `testAccNonzero` `CMP` to pay for, unlike `openBrTable`), load `N`
     *  (the clamp ceiling — one *past* the last real case, see below) into
     *  `SCRATCH_REG`, a placeholder local `BL` (recorded in
     *  `brTableHelperSites`, patched once `emitBrTableHelper` runs — this
     *  file's header), then `N + 1` literal halfword table slots: one per
     *  real case, plus one extra for a genuinely out-of-range selector.
     *
     *  That extra slot matters for more than tidiness: isa-core.md's own
     *  `acc ≥ N` behavior is "fall through, no case body runs at all, acc
     *  left untouched" — clamping to `N - 1` (re-running the *last* case)
     *  would silently diverge from that the moment a selector genuinely
     *  exceeds every case, executing code that shouldn't run at all.
     *
     *  Slot 0's target (right after the table) is known immediately and
     *  patched inline; slots 1..N-1 resolve the same way `nextCaseFixup`
     *  does above — the moment each preceding case's own `BLOCK_END`
     *  closes (`closeBlockEnd` below); the last slot (index `N`) resolves
     *  alongside `endFixups`, once the whole construct closes — so this
     *  function never needs to know where any case, or the construct's
     *  own end, actually lands. */
    openBrTableJump(e: Emitter, window: Window, n: number, accState: AccState): void
    {
        accState.flush(e, ACC_REG)
        arm.synthesizeImm32(SCRATCH_REG, n).forEach(w => e.emit(w))
        this.brTableHelperSites.push(e.placeholderBL())

        const base = e.pc // == lr, once the BL above actually executes
        const slots: number[] = []
        for(let i = 0; i <= n; i++) slots.push(e.emit(0))
        e.patchLiteral(slots[0]!, e.pc - base) // case 0 starts right here — no fixup needed

        this.frames.push({
            kind: "case", entryTos: window.tos, remaining: n,
            nextCaseFixup: null,
            table: { base, fixups: slots.slice(1, n), endSlot: slots[n]! },
            endFixups: [],
        })
    }

    openLoop(e: Emitter, window: Window): void
    {
        this.frames.push({ kind: "loopCond", entryTos: window.tos, loopStart: e.pc })
    }

    /** `isa-core.md §7.2`/§8.1: any TOS surplus above the block's own
     *  entry depth is dropped here, restoring r4-r7 to what the *target*
     *  depth's window mapping expects (window.ts's `restoreWindow`) before
     *  any of this function's own branch bookkeeping runs.
     *
     *  For a `case` frame specifically, `accState.flushLive` also runs
     *  here, before *any* of this case's own exit branches — every case
     *  is one linear, compile-time-sequential belief about `acc`, but at
     *  runtime any one of them (or none, for an out-of-range `BR_TABLE
     *  N>2` selector) could be the one that actually ran; merging back
     *  into shared code with something still `PENDING` would let the
     *  *next* case's own translation silently overwrite it before the
     *  merged code ever reads it (accstate.ts's own doc comment on
     *  `flushLive`). */
    closeBlockEnd(e: Emitter, window: Window, accState: AccState, loopExitCondition: arm.Condition | null): void
    {
        const top = this.frames[this.frames.length - 1]
        if(!top) throw new Error(`blocks: BLOCK_END with no open block`)

        if(top.kind === "case")
        {
            restoreWindow(e, window, top.entryTos)
            accState.flushLive(e, ACC_REG)
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
            else if(top.table !== null && top.table.fixups.length > 0)
            {
                // Same moment, same "resolve the next case's entry point"
                // job as `nextCaseFixup` above — just a raw table slot
                // (`patchLiteral`) instead of a branch instruction.
                const site = top.table.fixups.shift()!
                e.patchLiteral(site, e.pc - top.table.base)
            }
            if(top.remaining > 0)
                return // stay on this frame — now translating the next case
            for(const site of top.endFixups) e.patchBranch(site, e.pc)
            if(top.table !== null) e.patchLiteral(top.table.endSlot, e.pc - top.table.base)
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

/**
 * `BR_TABLE N>2`'s shared per-procedure dispatch routine (this file's
 * header) — reached by a local `BL` from every `openBrTableJump` site in
 * the same procedure, `lr` pointing at that call site's own table (the
 * whole reason this can be one shared routine: every call site supplies
 * its own table via `lr`, not this routine).
 *
 * `r0` (`ACC_REG`) holds the selector, `r2` (`SCRATCH_REG`) the clamp
 * ceiling `N` (one *past* the last real case — `openBrTableJump`'s own
 * `N + 1`-slot table) — both set by `openBrTableJump` immediately before
 * its `BL`. Deliberately computes the clamped index into `r1`
 * (`INDEX_REG`), never into `r0` itself: isa-core.md's own `acc ≥ N`
 * behavior leaves `acc` untouched (§7.1 — no case body runs at all), so
 * `r0` has to survive this routine unmodified on *both* paths, not just
 * the in-range one. Thumb-1 has no conditional move, so the clamp still
 * needs an actual branch: `r1 = r0` unconditionally first (the in-range
 * default), then `CMP r0,r2; BLS .ok; r1 = r2` overwrites it only when
 * `r0 > N`. `lr` itself can't be a register-offset `LDRH`'s base
 * (Thumb-1's 3-register addressing is low-register-only — this file's
 * `TABLE_PTR_REG` doc comment), so it's copied into `r3` first (then has
 * its own Thumb-mode bit cleared — the `SUBS r3,#1` below, own comment
 * has why); `r2` is reused (its clamp-ceiling role already done) for the
 * table entry (`case_i_start - lr`, `openBrTableJump`/`closeBlockEnd`'s
 * own convention), added back to reconstruct the absolute target — plus
 * the Thumb-mode bit, dropped by the earlier `SUBS` and restored right
 * before the final `BX` (this function's later comment has why). 11
 * instructions total, paid once per procedure regardless of how many
 * `BR_TABLE N>2` sites (or how large any single `N`) it has.
 */
export function emitBrTableHelper(e: Emitter): number
{
    const start = e.pc
    e.emit(arm.movHi(INDEX_REG, ACC_REG)) // default: in range, index = selector
    e.emit(arm.cmpReg(ACC_REG, SCRATCH_REG))
    const okSite = e.placeholderCondBranch(arm.Condition.LS)
    e.emit(arm.movHi(INDEX_REG, SCRATCH_REG)) // out of range: index = N (the extra slot) — r0 untouched either way
    e.patchBranch(okSite, e.pc)
    e.emit(arm.lslsImm(INDEX_REG, INDEX_REG, 1)) // halfword-indexed
    e.emit(arm.movHi(TABLE_PTR_REG, LR)) // lr -> low reg, for LDRH's addressing
    // `BL` always sets `lr` with bit 0 forced to 1 (the Thumb-mode marker
    // a later `BX`/`POP{PC}` needs) — harmless for a branch target
    // (hardware strips it when actually branching there) but fatal for
    // address arithmetic: `LDRH` needs a halfword-*aligned* (even)
    // address, and Cortex-M0 faults on an unaligned access, with no real
    // handler installed to recover from it (found the hard way: the
    // whole point of this being 1 bit off is a hang, not a wrong answer —
    // the fault handler spins). Since that bit is *always* exactly 1
    // right after a `BL`, subtracting 1 clears it for the `LDRH` below —
    // but the *final* target handed to `BX` needs that same bit set
    // again (confirmed the hard way too, via a QEMU instruction trace:
    // without it, `BX` reads bit 0 as "switch to ARM mode" instead of
    // "stay in Thumb," and starts decoding this same Thumb code as ARM
    // instructions), so it's added back once the table lookup is done —
    // clearing it was only ever about `LDRH`'s own addressing.
    e.emit(arm.subsImm8(TABLE_PTR_REG, 1))
    e.emit(arm.ldrh3(SCRATCH_REG, TABLE_PTR_REG, INDEX_REG))
    e.emit(arm.addsReg3(TABLE_PTR_REG, TABLE_PTR_REG, SCRATCH_REG))
    e.emit(arm.addsImm8(TABLE_PTR_REG, 1))
    e.emit(arm.bx(TABLE_PTR_REG))
    return start
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
 * materializes into `ACC_REG` first, always correct, occasionally one
 * instruction longer than optimal.
 */
export function emitComparison(e: Emitter, accState: AccState, op: BinaryOpcode, operand: Shape | undefined): arm.Condition
{
    const condition = DIRECT_CONDITION[op]
    if(condition === undefined) throw new Error(`blocks: ${op} is not a comparison`)

    let left = accState.peek()
    if(left.kind === "imm")
    {
        materializeShape(e, left, ACC_REG)
        left = { kind: "reg", reg: ACC_REG }
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
        materializeShape(e, operand, SCRATCH_REG)
        e.emit(arm.cmpReg(left.reg, SCRATCH_REG))
    }
    return condition
}

/**
 * The general case `emitComparison` deliberately doesn't cover: isa-core.md
 * §7.1/§7.2 make `BR_TABLE`/a `LOOP` condition's `BLOCK_END` lenient by
 * design — `acc < N` and `acc == 0` test *whatever value acc already
 * holds*, not specifically a comparison's 0/1 result. `while (n)` or
 * `if (flag)` never needs the lowerer to insert a normalizing comparison
 * ahead of the bare variable at all; that omitted instruction is exactly
 * what the leniency buys. So the immediately-preceding instruction need
 * not be a comparison — `n`'s own `LOAD`, or any other producer, is just
 * as valid a thing for `acc` to hold going into one of these.
 *
 * This is that general path: materialize whatever's pending, test it
 * against zero explicitly, and hand back `NE` (acc-is-nonzero) — the same
 * condition a genuine fused comparison's own "true" condition always
 * specializes, so callers never need to branch on which path produced it.
 * `emitComparison` above is strictly the *optimization*: skip this
 * explicit `CMP #0` (and the priced-in cost of materializing a 0/1 in the
 * first place) when the value was a comparison's result all along and
 * nothing else needed it materialized.
 */
export function testAccNonzero(e: Emitter, accState: AccState): arm.Condition
{
    accState.flush(e, ACC_REG)
    e.emit(arm.cmpImm8(ACC_REG, 0))
    return arm.Condition.NE
}
