/**
 * @ppl/jit-armv6m-prototype — block structure (docs/design.md §7.1/§7.2)
 *
 * Nesting is JS recursion, not an explicit stack: `openLoop`/`openBrTable`/
 * `openBrTableJump` return a `Frame` value instead of pushing one onto
 * anything, and translateProc.ts recurses into its own instruction-walking
 * function one level per open `LOOP`/`BR_TABLE`, holding that `Frame` as an
 * ordinary local instead of an array slot. The earlier design here was the
 * opposite — an explicit array, specifically so the translator wouldn't
 * need to recurse — but the property that actually has to transfer to a
 * no-heap native port isn't "no recursion," it's "bounded by nesting
 * depth": a real call stack is already there and already bounded, so
 * recursion maps onto it for free, while an explicit stack needs either a
 * heap (not available there) or a separately-derived max-nesting bound to
 * size a fixed array against (an analysis this doesn't need once the host
 * language's own call stack is the thing doing the counting — with its own
 * bound checked explicitly, translateProc.ts's `MAX_BLOCK_NESTING`, since
 * unlike a real embedded target's stack, blowing JS's own recursion limit
 * fails in a way that reveals nothing about *this* bound). Mirrors vm.ts's
 * own `BlockFrame` union in *shape* (case / loopCond / loopBody) —
 * repurposed here for backpatch bookkeeping instead of runtime dispatch.
 *
 * Confirms docs/design.md §16 item 5's own question: every branch this
 * translator emits resolves its target by the time its *own* enclosing
 * `BLOCK_END`/back-edge is reached — a `LOOP` back-edge target is already
 * known the instant `LOOP` opens (§7.2's own structure — the condition
 * block starts right there), and a `BR_TABLE` case's "skip to next
 * case"/"skip to end" targets resolve the moment that case (or the whole
 * construct) closes. No separate pass over the whole procedure is needed
 * for *this* — so item 5's out-of-range-conditional-branch concern
 * (`emitGuardedBranch`, below) is handled inline, at the one or two sites
 * that ever emit a fused conditional branch, by bounding the guarded span
 * *before* emitting it (a cheap, deliberately loose over-estimate,
 * `maxSpanBytes`) rather than by adding a genuine second pass.
 *
 * `BR_TABLE N ∈ {1, 2}` (isa-core.md §7.1's `if`/`if-else` forms) fuses
 * against a preceding comparison, branch-fusion style (`openBrTable`). `N >
 * 2` (a genuine multi-way selector, no comparison to fuse against) instead
 * compiles to a shared per-procedure jump-table helper (`openBrTableJump`/
 * `emitBrTableHelper`) — a small validation of docs/design.md §11's own
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
import { decodeInstr } from "./bytecodeReader"
import type { DecodedInstr } from "./bytecodeReader"
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

export type Frame =
    | {
        kind: "case"; entryTos: number; remaining: number
        nextCaseFixup: number | null
        /** Set only by `openBrTableJump` (`N > 2`): the jump table's own
         *  base offset (`lr`'s value once the dispatching `BL` runs), the
         *  address of the next not-yet-patched slot (case 0's slot is
         *  always patched immediately, needing no fixup at all; the rest
         *  sit contiguously right after it, so "next" is just a cursor,
         *  not a list — `nextFixupSlot === endSlot` means none remain),
         *  and the one extra slot — index `N`, beyond every real case —
         *  for a genuinely out-of-range selector (isa-core.md's own
         *  "acc ≥ N": falls through with *no* case body run at all, same
         *  target as `endFixupChain` below, patched alongside it). */
        table: { base: number; nextFixupSlot: number; endSlot: number } | null
        /** Head of a backpatch chain, not an array: every non-last case's
         *  own "skip to end" branch (this construct's only forward
         *  reference whose count actually scales with case count, not
         *  nesting depth) points at the *previous* pending one instead of
         *  its real target, using `patchBranch`'s own encoding as the
         *  link (`readBranchTarget` in emit.ts) — the first one in the
         *  chain points at itself, terminating it. `null` when no case
         *  has closed yet. */
        endFixupChain: number | null
      }
    | { kind: "loopCond"; entryTos: number; loopStart: number }
    | { kind: "loopBody"; entryTos: number; loopStart: number; exitFixup: number }

/**
 * §10.2/design.md §16 item 5: Thumb's conditional branch is an 8-bit
 * signed, ×2-scaled displacement — ±252 bytes (`armv6.ts`'s
 * `CBRANCH_A_BITS`/`CBRANCH_N_BITS`). A guarded span longer than that can't
 * be reached by a bare `condBranch` at all (`armv6.ts`'s `ioff` asserts
 * rather than silently emitting a wrong offset), so this can't be
 * discovered by trying — it has to be ruled out (or in) *before* the
 * branch is emitted, since the branch's own displacement field is fixed
 * the moment it's written.
 *
 * Rather than a genuine two-pass fixup (measure, then re-emit knowing real
 * sizes — a bigger architectural change than this translator's single
 * forward pass with backpatch chains), this bounds the span the cheap way:
 * walk the RTL instructions between `from` and the end of `blockCount`
 * sibling blocks (respecting nested `LOOP`/`BR_TABLE`, exactly like
 * `vm.ts`'s own `skipBlocks`/`skipConstruct`), summing a conservative
 * *maximum* native-byte cost per instruction — never tight, only ever safe
 * to overshoot, since underestimating is the only way this could go wrong
 * (and even then `armv6.ts`'s `ioff` would assert rather than silently
 * encode a wrong displacement).
 *
 * One flat bound would have to be sized for `CALL`'s real-ABI sequence
 * (this translator's single most expensive ordinary instruction — up to
 * two `synthesizeImm32` calls plus the window shuffle plus the dispatch
 * tail, topping out around 25 native instructions/50 bytes) and then get
 * paid by every other, far cheaper instruction too, making the "safe short
 * form" so pessimistic it would rarely fire for anything but trivially
 * short bodies. Scoring by opcode instead keeps the common case (ordinary
 * ALU/LOAD/STORE/PUSH/POP/comparison instructions, none of which come
 * anywhere near 16 bytes) usefully tight while still safely covering
 * `CALL` and a large `BR_TABLE N>2`'s own jump table (whose real size
 * scales with `N`, not a fixed constant).
 */
const ORDINARY_MAX_BYTES = 16
const CALL_MAX_BYTES = 64
/** `openBrTableJump`'s own fixed overhead (flush + `synthesizeImm32` +
 *  `placeholderBL`) before its `N + 1` two-byte table slots even start. */
const BR_TABLE_JUMP_OVERHEAD_BYTES = 32

function instrMaxBytes(instr: DecodedInstr): number
{
    if(instr.op === "CALL") return CALL_MAX_BYTES
    if(instr.op === "BR_TABLE" && instr.imm > 2) return BR_TABLE_JUMP_OVERHEAD_BYTES + (instr.imm + 1) * 2
    return ORDINARY_MAX_BYTES
}

/** `bytes` is the whole procedure's own raw bytecode (§16 item 16 — the
 *  same stream `translateProc.ts`'s main loop decodes from), `from` a
 *  byte offset into it. */
function maxSpanBytes(bytes: Uint8Array, from: number, blockCount: number): { bytes: number; nextPc: number }
{
    let pc = from
    let total = 0
    for(let remaining = blockCount; remaining > 0; remaining--)
    {
        for(;;)
        {
            if(pc >= bytes.length) throw new Error(`blocks: ran off the end of the procedure body while bounding a branch span`)
            const { instr, next } = decodeInstr(bytes, pc)
            total += instrMaxBytes(instr)
            if(instr.op === "BR_TABLE") { const sub = maxSpanBytes(bytes, next, instr.imm); total += sub.bytes; pc = sub.nextPc; continue }
            if(instr.op === "LOOP") { const sub = maxSpanBytes(bytes, next, 2); total += sub.bytes; pc = sub.nextPc; continue }
            pc = next
            if(instr.op === "BLOCK_END" || instr.op === "RETURN" || instr.op === "TRAP") break
        }
    }
    return { bytes: total, nextPc: pc }
}

/** Thumb's real conditional-branch reach is ±252 bytes; this stays well
 *  under that so `maxSpanBytes`'s own deliberate looseness never has to be
 *  exactly right, only safely conservative. */
const SAFE_COND_BRANCH_SPAN = 240

/** Emit a branch that's taken exactly when `condition` holds, reaching
 *  `blockCount` sibling blocks starting at byte offset `from` — a bare
 *  `condBranch` when `maxSpanBytes` proves that's provably in range, else
 *  the standard invert-and-long-branch idiom (branch on the *inverse*
 *  condition, in range by construction since it only ever skips the one
 *  instruction right after it — the real, wide-range `arm.b` placeholder
 *  this returns as `site`). Callers never need to know which shape they
 *  got: `emit.ts`'s `patchBranch`/`readBranchTarget` already dispatch on
 *  the site's own encoding. */
function emitGuardedBranch(e: Emitter, condition: arm.Condition, bytes: Uint8Array, from: number, blockCount: number): number
{
    if(maxSpanBytes(bytes, from, blockCount).bytes <= SAFE_COND_BRANCH_SPAN)
        return e.emit(arm.condBranch(condition, 0))

    const skip = e.emit(arm.condBranch(arm.inverse(condition), 0))
    const site = e.emit(arm.b(0))
    e.patchBranch(skip, skip + 4) // "not taken" (i.e. condition true) — fall through to the long branch right after
    return site
}

/** `isa-core.md §7.1`: `acc < N` executes `case[acc]`. Only the
 *  branch-fusion shape is implemented — `condition` is the *true*
 *  Thumb condition of whatever comparison immediately preceded this
 *  `BR_TABLE` (blocks.ts's own caller, translateProc.ts, is the one
 *  that knows that; this function only knows it's a condition). `body`/`pc`
 *  are only for `emitGuardedBranch`'s own span bound (§16 item 5) — this
 *  function itself still never looks past `pc`. */
export function openBrTable(e: Emitter, window: Window, n: number, condition: arm.Condition, bytes: Uint8Array, pc: number): Frame
{
    if(n !== 1 && n !== 2)
        throw new Error(`blocks: BR_TABLE ${n} not implemented — only if/if-else (N∈{1,2}, isa-core.md §7.1) are supported so far`)

    const frame: Frame = { kind: "case", entryTos: window.tos, remaining: n, nextCaseFixup: null, table: null, endFixupChain: null }
    const site = emitGuardedBranch(e, condition, bytes, pc, 1) // guards exactly case[0]'s own body
    if(n === 1)
    {
        e.patchBranch(site, site) // no case[1] to skip to — sole chain entry, self-linked to terminate
        frame.endFixupChain = site
    }
    else frame.nextCaseFixup = site // target is "start of case[1]", resolved the moment case[0] closes below
    return frame
}

/** `isa-core.md §7.1` for `N > 2`: a genuine multi-way selector, not a
 *  boolean — dispatches via a shared per-procedure helper
 *  (`emitBrTableHelper`) instead of a fused conditional branch. Emits,
 *  in order: materialize `acc`'s real value into `ACC_REG` (the
 *  helper needs the actual selector, not a condition — no
 *  `testAccNonzero` `CMP` to pay for, unlike `openBrTable`), load `N`
 *  (the clamp ceiling — one *past* the last real case, see below) into
 *  `SCRATCH_REG`, a placeholder local `BL` (returned as `helperSite` —
 *  translateProc.ts collects these across the whole procedure and patches
 *  them once `emitBrTableHelper` runs), then `N + 1` literal halfword
 *  table slots: one per real case, plus one extra for a genuinely
 *  out-of-range selector.
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
 *  alongside `endFixupChain`, once the whole construct closes — so this
 *  function never needs to know where any case, or the construct's
 *  own end, actually lands. */
export function openBrTableJump(e: Emitter, window: Window, n: number, accState: AccState): { frame: Frame; helperSite: number }
{
    accState.flush(e, ACC_REG)
    arm.synthesizeImm32(SCRATCH_REG, n).forEach(w => e.emit(w))
    const helperSite = e.placeholderBL()

    const base = e.pc // == lr, once the BL above actually executes
    for(let i = 0; i <= n; i++) e.emit(0) // n+1 slots, contiguous from `base` — no array needed, every address is `base + 2*i`
    e.patchLiteral(base, e.pc - base) // case 0 starts right here — no fixup needed

    const frame: Frame = {
        kind: "case", entryTos: window.tos, remaining: n,
        nextCaseFixup: null,
        table: { base, nextFixupSlot: base + 2, endSlot: base + 2 * n },
        endFixupChain: null,
    }
    return { frame, helperSite }
}

/** `isa-core.md §7.2`: the condition sub-block about to be translated is
 *  compiled exactly once but reached via two different runtime paths —
 *  this call site's own fall-through, and the body's own back-edge
 *  (`closeBlockEnd`'s `loopBody` case, below) — so whatever it folds as an
 *  operand has to mean the same thing on both. `flushLive` (tolerant of
 *  `POISONED`, same as a `case` boundary) forces both paths to arrive with
 *  `accState` in the identical state before either one reaches the
 *  condition's own first instruction — docs/design.md §16 item 3. */
export function openLoop(e: Emitter, window: Window, accState: AccState): Frame
{
    accState.flushLive(e, ACC_REG)
    return { kind: "loopCond", entryTos: window.tos, loopStart: e.pc }
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
 *  `flushLive`).
 *
 *  Returns the `Frame` translateProc.ts should keep looping with (the
 *  same one, mutated in place, for a case with more siblings left, or a
 *  fresh `loopBody` frame replacing the `loopCond` one that just closed),
 *  or `null` once this construct is genuinely done — the signal to
 *  return out of translateProc.ts's own recursive call for this level. */
/** The forward-branch bookkeeping a `case` frame's own close always needs
 *  — `nextCaseFixup`/the jump table's own next slot resolving to "wherever
 *  this case's own translated code ends," and (once the *last* case
 *  closes) `endFixupChain`/the table's own end slot resolving to the
 *  construct's shared end — regardless of *how* this case's own code
 *  actually ends. `emitSkipToEnd` is the one thing that genuinely differs
 *  between the two callers: a case that falls off the end of its own body
 *  (`closeBlockEnd`, below) has to actively branch past its own sibling
 *  cases' code, which immediately follows in memory; a case that ends via
 *  its own `RETURN`/`TRAP` instead (`closeCaseViaTerminator`, below) has
 *  already left the procedure entirely by the time this runs, so there's
 *  nothing left to skip past. */
function resolveCaseClose(e: Emitter, frame: Frame & { kind: "case" }, emitSkipToEnd: boolean): Frame | null
{
    frame.remaining -= 1
    if(frame.remaining > 0 && emitSkipToEnd)
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
        //
        // Threaded onto `endFixupChain` instead of pushed to an
        // array: this branch's own (still-unresolved) displacement
        // field is made to point at the previous pending site —
        // the first one in the chain points at itself — so walking
        // it back at the bottom needs only the one head pointer.
        const site = e.emit(arm.b(0))
        e.patchBranch(site, frame.endFixupChain ?? site)
        frame.endFixupChain = site
    }
    if(frame.nextCaseFixup !== null) { e.patchBranch(frame.nextCaseFixup, e.pc); frame.nextCaseFixup = null }
    else if(frame.table !== null && frame.table.nextFixupSlot !== frame.table.endSlot)
    {
        // Same moment, same "resolve the next case's entry point"
        // job as `nextCaseFixup` above — just a raw table slot
        // (`patchLiteral`) instead of a branch instruction. Slots
        // are contiguous and resolve strictly in order, so a
        // cursor suffices; no list to walk.
        e.patchLiteral(frame.table.nextFixupSlot, e.pc - frame.table.base)
        frame.table.nextFixupSlot += 2
    }
    if(frame.remaining > 0)
        return frame // stay on this frame — now translating the next case
    for(let site = frame.endFixupChain; site !== null;)
    {
        const prevSite = e.readBranchTarget(site)
        e.patchBranch(site, e.pc)
        site = prevSite === site ? null : prevSite
    }
    if(frame.table !== null) e.patchLiteral(frame.table.endSlot, e.pc - frame.table.base)
    return null
}

/** isa-core.md §4.5/§7.1: a `case`/loop body may close via a bare
 *  `RETURN`/`TRAP` instead of `BLOCK_END` — a terminator closes its own
 *  block "on its own" (vm.ts's own RETURN/TRAP handling is a bare
 *  `return`/`throw` that discards its whole `ctrl` stack in one go, no
 *  per-frame bookkeeping at all, since unwinding the interpreter's own
 *  call stack does it for free). translateProc.ts's own `Frame`
 *  bookkeeping has no such free ride: `nextCaseFixup`/a jump table's own
 *  next slot (and, on a *last* case, `endFixupChain`/the table's own end
 *  slot; for a loop, `exitFixup`) still have to resolve to *something*,
 *  or those branches stay permanently unpatched — a real, silent
 *  miscompilation, not just a missed bookkeeping step (confirmed the hard
 *  way: an unpatched short-form conditional branch's placeholder
 *  displacement of 0 decodes as "skip exactly one halfword," which can
 *  coincidentally land on the right answer for small enough test values
 *  without the mechanism actually being correct).
 *
 *  The reconciliation a *normal* close performs for the shared
 *  fall-through path (`restoreWindow`'s real pop/sp-adjust,
 *  `accState.flushLive`'s real materialize, a non-last case's "skip to
 *  end" branch) is moot here: the terminator's own emitted return
 *  sequence has already left the procedure, so nothing downstream in the
 *  emitted code is ever reached by falling out of *this* block. Only the
 *  bookkeeping half survives — `window`/`accState` still need resetting
 *  to what's actually true at whatever's reached *next* (a case's own
 *  sibling, or the code after the whole construct): exactly this
 *  construct's own entry state, untouched by this block's own body, since
 *  nothing of that body ever runs on the path that reaches "next" either. */
export function closeCaseViaTerminator(e: Emitter, window: Window, accState: AccState, frame: Frame): Frame | null
{
    if(frame.kind !== "case") throw new Error(`blocks: closeCaseViaTerminator on a non-case frame`)
    window.tos = frame.entryTos
    accState.setClean(ACC_REG)
    return resolveCaseClose(e, frame, false)
}

/** `closeCaseViaTerminator`'s own doc comment, but for a `LOOP`'s body
 *  (isa-core.md §7.2's own explicit allowance: "a loop that tests its
 *  condition once, then either runs its body once and exits via
 *  RETURN/TRAP or falls through, never taking the back-edge"). Unlike a
 *  `case`, there's no "more siblings to translate" branch — a loop has
 *  exactly one body, so this always fully closes the construct; the only
 *  bookkeeping left is patching `exitFixup` (the condition's own
 *  cond-false exit branch, emitted back when the condition block closed)
 *  to land right here. */
export function closeLoopBodyViaTerminator(e: Emitter, window: Window, accState: AccState, frame: Frame): void
{
    if(frame.kind !== "loopBody") throw new Error(`blocks: closeLoopBodyViaTerminator on a non-loopBody frame`)
    e.patchBranch(frame.exitFixup, e.pc)
    window.tos = frame.entryTos
    accState.setClean(ACC_REG)
}

export function closeBlockEnd(e: Emitter, window: Window, accState: AccState, frame: Frame, loopExitCondition: arm.Condition | null, bytes: Uint8Array, pc: number): Frame | null
{
    if(frame.kind === "case")
    {
        restoreWindow(e, window, frame.entryTos)
        accState.flushLive(e, ACC_REG)
        return resolveCaseClose(e, frame, true)
    }

    if(frame.kind === "loopCond")
    {
        restoreWindow(e, window, frame.entryTos)
        if(loopExitCondition === null) throw new Error(`blocks: LOOP condition block closed with no fused comparison to branch on`)
        // `pc` is this BLOCK_END's own position — the body (what this exit
        // branch guards, §16 item 5) starts right after it.
        const exitFixup = emitGuardedBranch(e, loopExitCondition, bytes, pc + 1, 1)
        return { kind: "loopBody", entryTos: frame.entryTos, loopStart: frame.loopStart, exitFixup }
    }

    // loopBody: unconditional back-edge, then the earlier exit branch
    // resolves to right after it — both targets were knowable without
    // ever looking past this point (see this file's header). The same
    // flushLive openLoop's own fall-through does (§16 item 3) — the
    // condition sub-block's compiled code can only assume a single,
    // consistent incoming accState if *this* path forces it too.
    restoreWindow(e, window, frame.entryTos)
    accState.flushLive(e, ACC_REG)
    e.emit(arm.b(frame.loopStart - (e.pc + 4)))
    e.patchBranch(frame.exitFixup, e.pc)
    return null
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

/** `DIRECT_CONDITION`, but for the operands swapped — the condition that's
 *  true for `right OP left` exactly when `left OP right` is (isa-core.md
 *  §10.1's own mirror-table optimization). `EQ`/`NE` mirror to themselves;
 *  every ordering comparison mirrors to its opposite direction, signedness
 *  preserved (`LT_S` swaps with `GT_S`, `LT_U` with `GT_U`, never across
 *  the sign line). */
const MIRRORED_CONDITION: Partial<Record<BinaryOpcode, arm.Condition>> = {
    EQ: arm.Condition.EQ, NE: arm.Condition.NE,
    LT_S: arm.Condition.GT, LE_S: arm.Condition.GE, GT_S: arm.Condition.LT, GE_S: arm.Condition.LE,
    LT_U: arm.Condition.HI, LE_U: arm.Condition.HS, GT_U: arm.Condition.LO, GE_U: arm.Condition.LS,
}

/**
 * Emit the `CMP` for a comparison whose *only* consumer is the following
 * `BR_TABLE`/`LOOP`-condition `BLOCK_END` — never materializes a 0/1
 * result (that's the whole point of this fusion axis). Returns the Thumb
 * condition that's true exactly when the comparison itself is true; the
 * caller (translateProc.ts) hands that to `openBrTable`/`closeBlockEnd`
 * directly or inverts it first, depending which side needs "true".
 *
 * §10.1's immediate-side mirror-table optimization: acc holding an
 * immediate that fits `CMP`'s own 8-bit field, compared against a register
 * operand, doesn't need materializing at all — `operand CMP #acc` (Thumb's
 * only encodable order, register first) with the mirrored condition is the
 * exact same test as `acc OP operand`, one instruction instead of two.
 * Doesn't apply when the immediate doesn't fit (still has to materialize
 * *something*, so there's nothing to save) or when `operand` is itself an
 * immediate (nothing on the right to use as `CMP`'s register operand).
 */
export function emitComparison(e: Emitter, accState: AccState, op: BinaryOpcode, operand: Shape | undefined): arm.Condition
{
    const condition = DIRECT_CONDITION[op]
    if(condition === undefined) throw new Error(`blocks: ${op} is not a comparison`)

    if(operand === undefined)
        throw new Error(`blocks: PEEK_PEEK comparison fusion is not implemented (not exercised by this corpus)`)

    let left = accState.peek()

    if(left.kind === "imm" && operand.kind === "reg" && arm.fitsImm8(left.value))
    {
        e.emit(arm.cmpImm8(operand.reg, left.value))
        return MIRRORED_CONDITION[op]!
    }

    if(left.kind === "imm")
    {
        materializeShape(e, left, ACC_REG)
        left = { kind: "reg", reg: ACC_REG }
    }

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

/**
 * Materialize a comparison's boolean result (0 or 1) into `dest` — the
 * general case `emitComparison` deliberately doesn't cover (its own doc
 * comment: "never materializes a 0/1 result... the whole point of this
 * fusion axis"). Needed whenever a comparison is used as an ordinary
 * value rather than a branch's own condition (design.md §16 item 8): the
 * DSL/RTL/VM already treat a comparison as an ordinary value-producing
 * instruction (isa-core.md §4.2, `vm.ts`'s own `evalBinary` — `EQ`/`LT_S`/
 * etc. write a real 0/1 the same way `ADD` writes its own result); this
 * translator's fusion-only assumption was the gap, not the ISA.
 *
 * Thumb-1 has no conditional-select, so this costs the `CMP`
 * `emitComparison` already emits plus four more: branch straight to the
 * "false" case, set `dest` to `1` and jump past it, or fall through and
 * set `dest` to `0` — design.md §10's own "about 4-5 instructions"
 * estimate for exactly this idiom. The branch has to be the *very* next
 * thing after `emitComparison`'s own `CMP`, nothing in between: a `MOVS`
 * (which updates N/Z, exactly what a comparison's own condition reads)
 * would silently make the branch test the `MOVS`'s own result instead of
 * the comparison's — this is why `dest` can't be pre-set to `1` before
 * calling `emitComparison` either, tempting as the one-instruction
 * saving looks: `emitComparison` may itself need to materialize a
 * pending value *into* `ACC_REG` before its own `CMP`, and if `dest` is
 * `ACC_REG` (the common no-fold case), that materialization would
 * clobber the pre-set `1` before the comparison even runs.
 */
export function materializeComparison(e: Emitter, accState: AccState, op: BinaryOpcode, operand: Shape | undefined, dest: number): void
{
    const trueCondition = emitComparison(e, accState, op, operand)
    const falseSite = e.placeholderCondBranch(arm.inverse(trueCondition))
    e.emit(arm.movsImm8(dest, 1))
    const endSite = e.placeholderBranch()
    e.patchBranch(falseSite, e.pc)
    e.emit(arm.movsImm8(dest, 0))
    e.patchBranch(endSite, e.pc)
}
