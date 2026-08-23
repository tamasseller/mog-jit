/**
 * @ppl/jit-armv6m-prototype — per-procedure translation (docs/design.md §10)
 *
 * The single instruction-at-a-time sink the whole prototype is organized
 * around: one forward `pc` walk over `proc.body`'s own raw bytecode
 * (`bytecodeReader.ts`'s `decodeInstr`, docs/design.md §16 item 16 — `pc`
 * is a byte offset, decoded on demand, never a pre-decoded `RtlInstr[]`
 * walked by array index), dispatching each instruction to whichever piece
 * of state it belongs to — window.ts's `tos` counter, accstate.ts's
 * CLEAN/PENDING slot, or blocks.ts's open blocks (one recursive
 * `translateBody` call per nesting level, not an explicit stack) — never
 * more than a one-token lookahead (the STORE-fold peek), matching §10.1's
 * own "no lookahead past that" argument.
 *
 * Scope (see the package README/this session's notes): §6's calling-
 * convention shuffle (`CALL`, below) was this design's own least-proven
 * piece before call.test.ts (and abi-dispatch.test.ts/eviction.test.ts
 * after it) verified it end to end on real QEMU — implemented here using
 * the whole-program up-front layout this prototype already has (no
 * dispatch table, no eviction, so a plain `BL` stands in for §9's real
 * `BLX`-through-a-table;
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
 * boundary (§5). `EXT` still throws (unimplemented). Unary ops
 * (`NEG`/`NOT`/`CLZ`/`REVBITS`, `unaryops.ts`) and comparisons used as
 * ordinary values rather than a branch's own condition (`blocks.ts`'s
 * `materializeComparison`) are implemented (docs/design.md §16 item 8).
 */

import { Emitter } from "./emit"
import { Window, physReg, inWindow, spillForCall, fillCalleeArgs, reloadAfterCall } from "./window"
import { AccState, emitBinary } from "./accstate"
import { Shape } from "./shape"
import { ACC_REG, SCRATCH_REG, ENTRY_IDX_REG, ENTRY_OFFSET_REG, ENTRY_JUMP_REG, HELPER_VEC_REG, WINDOW_SIZE } from "./registers"
import { Frame, openBrTable, openBrTableJump, openLoop, closeBlockEnd, closeCaseViaTerminator, closeLoopBodyViaTerminator, emitComparison, materializeComparison, isComparisonOp, testAccNonzero, emitBrTableHelper } from "./blocks"
import { emitUnary, newUnaryHelperSites, emitClzHelper, emitRevbitsHelper } from "./unaryops"
import { decodeInstr } from "./bytecodeReader"
import type { DecodedInstr } from "./bytecodeReader"
import * as arm from "./armv6"
import * as runtime from "./runtime"
import { encodeBody } from "@ppl/machine"
import type { RtlProc, ComboName } from "@ppl/machine"

const LR = 14

/** Bounds the recursion `LOOP`/`BR_TABLE` nesting uses in place of an
 *  explicit block stack (blocks.ts's own header has why) — the check a
 *  real embedded target's own finite call stack would need too, bailing
 *  out with `RESOURCE_ERROR` instead of recursing further. Here it just
 *  throws: this prototype has no such error-reporting path of its own,
 *  and JS's own recursion limit sits far above any realistic procedure's
 *  actual nesting depth, so this is the bound that actually matters. */
const MAX_BLOCK_NESTING = 32

/** How a `CALL`/`RETURN`/procedure entry actually gets compiled — the one
 *  seam between the window/accstate/binops/blocks machinery below (which
 *  doesn't care how a call is dispatched) and the two very different
 *  call/return mechanisms this prototype now supports: the default
 *  no-eviction shape (a plain `BL`/`pop{pc}`, this file's own original
 *  simplification) and the real runtime ABI (docs/jit-armv6m-dispatch-
 *  handoff.html §06 — `callHelper`/`returnHelper`, no linking pass at all).
 */
export interface CallReturnStrategy
{
    /** Emitted unconditionally as this procedure's own literal first bytes. */
    emitPrologue(e: Emitter): void
    /** Emit a full `CALL` dispatch to `calleeIndex` — the register window
     *  is already shuffled into the callee's canonical phase-0 layout by
     *  the time this runs. Returns whatever program.ts's own linking pass
     *  needs recorded, or `null` if nothing needs linking later (every
     *  value the ABI-real strategy emits is already a compile-time
     *  constant, §9/§11). */
    emitCall(e: Emitter, calleeIndex: number): CallSite | null
    /** Emit the return sequence — `sp` is already rebalanced
     *  (`discardWindow`) and the return value already sits in `ACC_REG`. */
    emitReturn(e: Emitter): void
}

/** Whether a procedure's own body needs `LR` protected before anything can
 *  clobber it — a nested `CALL` (either strategy below), `blocks.ts`'s
 *  own `emitBrTableHelper` (`BR_TABLE N > 2` only), or `unaryops.ts`'s
 *  `CLZ`/`REVBITS` helpers, all reached by a local `BL` that clobbers
 *  real hardware `LR` directly, independently of whichever call/return
 *  mechanism is in play. Shared by both strategies and by `translateProc`'s
 *  own `Window` construction (below) — one predicate, not separate copies
 *  that could drift.
 *
 *  `override`, when supplied, wins outright — docs/design.md §16 item 15's
 *  wiring: `procDirectory.ts`'s `buildProcDirectory` already derives this
 *  exact fact from raw wire bytes (`ProcDirEntry.savesLR`), so a caller
 *  that already has a directory (program.ts's own driver) hands its
 *  entry straight through instead of paying for a second, redundant
 *  `RtlInstr[]` scan of the same body. Every direct `translateProc`/
 *  `noEvictionStrategy`/`abiRealStrategy` call outside that one driver —
 *  every existing unit test — has no directory at all, so the scan stays
 *  the fallback rather than a hard requirement. */
function needsLRSave(proc: RtlProc, override?: boolean): boolean
{
    if(override !== undefined) return override
    return proc.body.some(i => i.op === "CALL" || (i.op === "BR_TABLE" && i.imm > 2) || i.op === "CLZ" || i.op === "REVBITS")
}

/** Today's default: `CALL` compiles to a plain, whole-program-linked `BL`
 *  (program.ts patches it once every procedure's layout is known) and
 *  `RETURN` to `pop{..,pc}`/`bx lr` — a faithful simplification for a
 *  translator with no dispatch table, no eviction (this file's own header).
 */
export function noEvictionStrategy(proc: RtlProc, savesLROverride?: boolean): CallReturnStrategy
{
    const savesLR = needsLRSave(proc, savesLROverride)
    // Original arguments that never fit in the window (`Window`'s own
    // `initialSpilledCount`) sit *below* this procedure's own `push{lr}` —
    // `pop{pc}` alone only reclaims the `lr` word itself, so a genuinely
    // deep-args non-leaf procedure needs the return address in a plain
    // register first, the remainder reclaimed explicitly, then a real
    // `bx` — the one shape `pop{pc}`'s single instruction can't cover.
    const initialSpilledCount = Math.max(0, proc.argCount - WINDOW_SIZE)
    return {
        emitPrologue(e) { if(savesLR) e.emit(arm.pushWithLr([])) },
        emitCall(e, calleeIndex) { return { siteOffset: e.placeholderBL(), calleeIndex } },
        emitReturn(e)
        {
            if(!savesLR) { e.emit(arm.bx(LR)); return }
            if(initialSpilledCount === 0) { e.emit(arm.popWithPc([])); return }
            e.emit(arm.pop([SCRATCH_REG]))
            e.emit(arm.incrSp(4 * initialSpilledCount))
            e.emit(arm.bx(SCRATCH_REG))
        },
    }
}

/** The real runtime ABI (docs/design.md §9):
 *  every compiled procedure starts with the fixed prologue stub
 *  (runtime.ts), `CALL` pushes `REC(procIdx, K+1)` into `lr` and
 *  tail-jumps into `callHelper`, `RETURN` tail-jumps into one of
 *  `qemu/runtime.S`'s two `returnHelper` variants — `lr` is otherwise dead
 *  in this ABI (this JIT never emits hardware `BL`/`BLX` in its own
 *  compiled code), so the record travels there instead of on the stack,
 *  the standard AAPCS leaf/non-leaf convention. `savesLR` procedures
 *  (this one, or any of its callees, makes at least one nested `CALL` —
 *  `needsLRSave` above) additionally `push{lr}` in their own prologue
 *  before their first nested `CALL` can clobber it, and pick the
 *  stack-fed `returnHelper` variant instead of the `lr`-fed one — the
 *  *only* place this ABI's register shuffle is duplicated per procedure at
 *  all; `callHelper`/`returnHelper` themselves carry it once, in flash
 *  (`qemu/runtime.S`), not once per call/return site. `procIdx` is this
 *  procedure's own index (needed for the record on every `CALL`).
 */
export function abiRealStrategy(procIdx: number, proc: RtlProc, savesLROverride?: boolean): CallReturnStrategy
{
    const savesLR = needsLRSave(proc, savesLROverride)
    // See noEvictionStrategy's own comment: original out-of-window
    // arguments sit below this procedure's own push{lr}, so retrieving the
    // record and reclaiming that remainder can't both happen inside one
    // shared, parameterless returnHelper variant — emitReturn's third
    // branch below does both inline, once per procedure that needs it
    // (rare: non-leaf *and* argCount > WINDOW_SIZE), then tail-jumps into
    // the bare shared tail instead.
    const initialSpilledCount = Math.max(0, proc.argCount - WINDOW_SIZE)

    function buildCallSequence(calleeIndex: number, k: number): number[]
    {
        const record = runtime.packRecord(procIdx, k + 1)
        return [
            ...arm.synthesizeImm32(ENTRY_IDX_REG, record),
            ...(arm.fitsImm8(calleeIndex)
                ? [arm.movsImm8(ENTRY_OFFSET_REG, calleeIndex)]
                : arm.synthesizeImm32(ENTRY_OFFSET_REG, calleeIndex)),
            arm.movHi(ENTRY_JUMP_REG, HELPER_VEC_REG),
            arm.ldr5(ENTRY_JUMP_REG, ENTRY_JUMP_REG, 0), // callHelper, index 0 — its own `mov lr,r1` picks up the record from r1
            arm.bx(ENTRY_JUMP_REG),
        ]
    }

    return {
        emitPrologue(e)
        {
            runtime.emitPrologueStub().forEach(w => e.emit(w))
            if(savesLR) e.emit(arm.pushWithLr([]))
        },
        emitCall(e, calleeIndex)
        {
            // K is a byte offset from the procedure's own body start (past
            // the fixed-size stub, runtime.ts's own header) to *this*
            // sequence's own resume point — which depends on how many
            // instructions this same sequence takes to encode K itself
            // (`REC`'s packed immediate). Fixed-point, not two-pass: stable
            // in one or two iterations for any realistic procedure size
            // (validated against real QEMU before this was wired in).
            // Unaffected by the prologue's own conditional `push{lr}`
            // above: `e.pc` already reflects it, same as it would any other
            // emitted instruction — STUB_SIZE only ever measures from right
            // after the fixed stub, never from after the whole prologue.
            const preCallPc = e.pc
            const k = fixedPoint(guess =>
                (preCallPc - runtime.STUB_SIZE) + buildCallSequence(calleeIndex, guess).length * 2, 0)
            buildCallSequence(calleeIndex, k).forEach(w => e.emit(w))
            return null
        },
        emitReturn(e)
        {
            // The common cases (leaf, or non-leaf with argCount ≤
            // WINDOW_SIZE) are the same 3-instruction dispatch tail either
            // way — only the helper-vector slot differs: index 1
            // (`returnHelperFromLr`) reads the record straight out of
            // `lr`; index 2 (`returnHelperFromStack`) `pop`s it, undoing
            // this procedure's own prologue `push{lr}`. Neither variant's
            // own fetch step is emitted here — qemu/runtime.S carries it
            // once, shared, not once per `RETURN`/`TRAP` site.
            if(savesLR && initialSpilledCount > 0)
            {
                // The rare combination: retrieve the record *and* reclaim
                // the original out-of-window arguments below it — neither
                // shared variant can do this alone (see this function's own
                // comment above) — then tail-jump into the bare shared tail
                // (index 3), which only unpacks/dispatches r1.
                e.emit(arm.pop([ENTRY_IDX_REG]))
                e.emit(arm.incrSp(4 * initialSpilledCount))
                e.emit(arm.movHi(ENTRY_JUMP_REG, HELPER_VEC_REG))
                e.emit(arm.ldr5(ENTRY_JUMP_REG, ENTRY_JUMP_REG, 12)) // returnHelperTail, index 3
                e.emit(arm.bx(ENTRY_JUMP_REG))
                return
            }
            e.emit(arm.movHi(ENTRY_JUMP_REG, HELPER_VEC_REG))
            e.emit(arm.ldr5(ENTRY_JUMP_REG, ENTRY_JUMP_REG, savesLR ? 8 : 4))
            e.emit(arm.bx(ENTRY_JUMP_REG))
        },
    }
}

function fixedPoint(f: (guess: number) => number, initial: number, maxIters = 5): number
{
    let guess = initial
    for(let i = 0; i < maxIters; i++)
    {
        const next = f(guess)
        if(next === guess) return guess
        guess = next
    }
    throw new Error("translateProc: CALL resume offset failed to converge")
}

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

/** Slot k's window register a peek at the instruction starting at byte
 *  offset `nextPc` resolves to, if that instruction is a `STORE`
 *  targeting a currently in-window slot — the one-token destination-fold
 *  trigger every producer/consumer below checks before falling back to
 *  `ACC_REG`. `nextPc` is always the decoded `.next` of whatever
 *  instruction is being considered for the fold (§16 item 16 — a byte
 *  offset, not a fixed `pc + 1` array index, since instructions are
 *  different lengths on the wire). Out-of-window `STORE` targets don't
 *  fold: there's no register to fold into (the target lives on the real
 *  stack, `spillOffset` below), so the fold is skipped and the ordinary
 *  `STORE` case (which does know how to reach it) handles it on the next
 *  iteration instead. Returns the `STORE`'s own `.next` alongside its
 *  register, so a caller that takes the fold can jump `pc` straight
 *  there instead of re-decoding to find it. */
function peekStoreFold(bytes: Uint8Array, nextPc: number, tos: number): { reg: number; afterNext: number } | null
{
    if(nextPc >= bytes.length) return null
    const { instr, next } = decodeInstr(bytes, nextPc)
    if(instr.op === "STORE" && inWindow(tos, instr.target)) return { reg: physReg(instr.target), afterNext: next }
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
 *  indexes it.
 *
 *  `savesLROverride`, when supplied, must be the *same* value the caller
 *  already gave (or will give) `strategy`'s own constructor — this
 *  function's own `Window` needs to agree with whichever call/return
 *  sequence `strategy` actually emits about whether `lr` got saved, and
 *  nothing here re-derives `strategy`'s own choice to cross-check it.
 *  `program.ts` is the one caller that supplies this at all (§16 item
 *  15's wiring); every direct test-file call leaves it `undefined`,
 *  falling back to `needsLRSave`'s own scan exactly as before. */
export function translateProc(
    proc: RtlProc,
    calleeArgCounts: readonly number[] = [],
    strategy: CallReturnStrategy = noEvictionStrategy(proc),
    savesLROverride?: boolean,
): TranslatedProc
{
    const e = new Emitter()
    const window = new Window(proc.argCount, needsLRSave(proc, savesLROverride))
    const accState = new AccState()
    const callSites: CallSite[] = []
    const brTableHelperSites: number[] = []
    const unaryHelperSites = newUnaryHelperSites()
    // §16 item 16: the main loop below decodes one instruction at a time
    // from this raw byte stream (bytecodeReader.ts's `decodeInstr`) —
    // never a pre-decoded `RtlInstr[]` array walked by index. Encoding
    // `proc.body` once, here, rather than accepting bytes directly, keeps
    // every existing caller (every test file, `program.ts`) unchanged;
    // what's genuinely different is that nothing below ever indexes
    // `proc.body` again past this line.
    const bytes = encodeBody(proc.body)

    // prologue — strategy-defined (noEvictionStrategy: `lr` saved only if
    // this body needs to, since a `CALL` compiles to a plain `BL`;
    // abiRealStrategy: the fixed-size dispatch-table prologue stub,
    // unconditionally). No fixed spill-area reservation either way
    // (window.ts's own header): every spill/fill is a real sp-adjusting
    // PUSH/POP, so sp naturally tracks actual depth with nothing reserved
    // up front.
    strategy.emitPrologue(e)

    let pc = 0

    // §6 callee-side prologue: the last argument (if any) arrives in acc
    // — ACC_REG, by this prototype's own native ABI choice, matching the
    // doc's own convention exactly. §6's "as a fold" optimization
    // (docs/design.md §16 items 13/14 merged): rather than unconditionally
    // flushing it into phys(argCount-1) regardless of whether the body
    // ever reads that slot by index, leave it PENDING (a fresh producer,
    // exactly like a CALL's own return value) whenever that's provably
    // safe. Proven by a whole-body reference count, not a one-token
    // lookahead — the window's own invariant ("phys(k) holds slot k's
    // value whenever k is in-window") has to hold for the *entire rest*
    // of the procedure once this slot is populated by anything other
    // than this flush, not just the next instruction:
    //   - zero references anywhere: the argument may still be read via
    //     acc itself (RETURN, further arithmetic, a STORE-fold target) —
    //     just never through an explicit LOAD/STORE/register-mode
    //     operand naming this exact slot — so phys(argCount-1) never
    //     needs to hold anything at all.
    //   - exactly one reference, and it's body[0]'s own LOAD of this
    //     exact slot: eliding both the flush and that LOAD is safe for
    //     the same reason (this was the only place that would ever have
    //     read phys(argCount-1)) — the common shape for any procedure
    //     that uses its last argument immediately.
    // Every other shape (multiple references, or the one reference
    // anywhere but body[0]'s own LOAD) falls back to the unconditional
    // flush this always did — never assumed, only proven.
    if(proc.argCount >= 1)
    {
        const lastArgSlot = proc.argCount - 1
        const refs: number[] = [] // byte offsets, not array indices (§16 item 16)
        for(let p = 0; p < bytes.length;)
        {
            const { instr, next } = decodeInstr(bytes, p)
            if("target" in instr && instr.target === lastArgSlot) refs.push(p)
            p = next
        }

        if(refs.length === 0)
        {
            accState.producer({ kind: "reg", reg: ACC_REG })
        }
        else if(refs.length === 1 && refs[0] === 0 && decodeInstr(bytes, 0).instr.op === "LOAD")
        {
            accState.producer({ kind: "reg", reg: ACC_REG })
            pc = decodeInstr(bytes, 0).next
        }
        else
        {
            accState.flush(e, physReg(lastArgSlot))
        }
    }

    let pendingComparisonCondition: arm.Condition | null = null

    function returnSequence(): void
    {
        // Unwind whatever this body spilled — nothing downstream reads
        // r4-r7 again, so there's nothing to reload for (window.ts's
        // `Window.discardWindow`), only sp to rebalance before the
        // strategy's own return sequence runs.
        window.discardWindow(e)
        strategy.emitReturn(e)
    }

    // isa-core.md §4.5/§7.1/§7.2: a `case`/loop body may close via a bare
    // RETURN/TRAP instead of BLOCK_END — blocks.ts's `closeCaseViaTerminator`/
    // `closeLoopBodyViaTerminator` own doc comments have the full story.
    // Mirrors the BLOCK_END case's own `closeBlockEnd`/`frame = next`
    // dispatch below, just triggered by a terminator instead.
    function closeFrameForTerminator(f: Frame): Frame | null
    {
        if(f.kind === "case") return closeCaseViaTerminator(e, window, accState, f)
        closeLoopBodyViaTerminator(e, window, accState, f)
        return null
    }

    // Block nesting is JS recursion, not an explicit stack — one call per
    // open `LOOP`/`BR_TABLE`, its own `Frame` held as a plain local
    // (blocks.ts's own header has why). `frame` is this level's own open
    // block, `null` at the top level; `depth` is checked against
    // `MAX_BLOCK_NESTING` above before recursing any further.
    function translateBody(frame: Frame | null, depth: number): void
    {
        if(depth > MAX_BLOCK_NESTING)
            throw new Error(`translateProc: block nesting exceeds ${MAX_BLOCK_NESTING} (pc=${pc})`)

        while(pc < bytes.length)
        {
            // §16 item 16: one instruction decoded on demand from the raw
            // byte stream — `afterInstr` (a byte offset, not `pc + 1`)
            // replaces every bare `pc++` below; a fold that consumes a
            // second instruction jumps `pc` to *that* instruction's own
            // decoded `.next` instead of a fixed `pc += 2`, since
            // instructions are different lengths on the wire.
            const { instr, next: afterInstr } = decodeInstr(bytes, pc)

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

                const site = strategy.emitCall(e, instr.calleeIndex)
                if(site) callSites.push(site)

                // The callee has freely clobbered r4-r7 for its own,
                // unrelated window — reload the caller's own, now that
                // `stackArgs` slots have been consumed by the call.
                reloadAfterCall(e, window, window.tos - stackArgs)

                // The return value is now in acc — a fresh producer, same
                // as any other (LOAD/POP/CONST), so a following STORE
                // still folds.
                accState.producer({ kind: "reg", reg: ACC_REG })
                pc = afterInstr
                continue
            }

            case "PUSH":
                window.pushValue(e, accState)
                pc = afterInstr
                continue

            case "POP":
                e.emit(arm.movHi(ACC_REG, window.topReg)) // materialize now — see accstate.ts's header for why a bare POP can't safely stay PENDING
                accState.setClean(ACC_REG)
                window.finishPop(e) // must run after the read above — same register
                pc = afterInstr
                continue

            case "NEG": case "NOT": case "CLZ": case "REVBITS":
            {
                // No fold axis of its own (unaryops.ts's own header) —
                // always flush first, exactly like the general binary-op
                // "no match in the table" fallback (§10.1).
                accState.flush(e, ACC_REG)
                const fold = peekStoreFold(bytes, afterInstr, window.tos)
                const dest = fold?.reg ?? ACC_REG
                emitUnary(e, instr.op, dest, unaryHelperSites)
                accState.setClean(dest)
                pc = fold?.afterNext ?? afterInstr
                continue
            }

            case "BLOCK_END":
            {
                if(frame === null) throw new Error(`translateProc: BLOCK_END with no open block (pc=${pc})`)
                let loopExitCond: arm.Condition | null = null
                if(frame.kind === "loopCond")
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
                const next = closeBlockEnd(e, window, accState, frame, loopExitCond, bytes, pc)
                pc = afterInstr
                if(next === null) return
                frame = next
                continue
            }

            case "LOOP":
                pc = afterInstr
                translateBody(openLoop(e, window, accState), depth + 1)
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
                const n = instr.imm
                pc = afterInstr
                if(n > 2)
                {
                    const opened = openBrTableJump(e, window, n, accState)
                    brTableHelperSites.push(opened.helperSite)
                    pendingComparisonCondition = null
                    translateBody(opened.frame, depth + 1)
                }
                else
                {
                    const trueCondition = pendingComparisonCondition ?? testAccNonzero(e, accState)
                    pendingComparisonCondition = null
                    translateBody(openBrTable(e, window, n, trueCondition, bytes, pc), depth + 1)
                }
                continue
            }

            case "RETURN":
                accState.flush(e, ACC_REG) // isa-core.md §7: the return value is whatever's in acc
                returnSequence()
                pc = afterInstr
                // isa-core.md §7.1/§7.2: a case/loop body may be closed by
                // a bare terminator instead of BLOCK_END — this one just
                // served as that closer, so whatever frame is currently
                // open needs the same forward-branch bookkeeping BLOCK_END
                // would have triggered (closeFrameForTerminator, above),
                // then either keeps translating sibling cases (`continue`)
                // or unwinds the recursive call opened for this construct
                // (`return`) exactly like the BLOCK_END case below does.
                if(frame !== null)
                {
                    const next = closeFrameForTerminator(frame)
                    if(next === null) return
                    frame = next
                }
                continue

            case "TRAP":
                // §12's real Report/error model isn't implemented — sentinel-
                // encode the trap so the QEMU trampoline (test/qemu-run.ts)
                // can tell it apart from a normal return: high bit set, low
                // bits the trap code. Lands in ACC_REG for the same reason
                // RETURN's own value does — it's what the harness reads.
                arm.synthesizeImm32(ACC_REG, 0x80000000 | instr.imm).forEach(w => e.emit(w))
                returnSequence()
                pc = afterInstr
                if(frame !== null) // same as RETURN, above
                {
                    const next = closeFrameForTerminator(frame)
                    if(next === null) return
                    frame = next
                }
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
                    e.emit(arm.ldrSp(ACC_REG, window.spillOffset(instr.target)))
                    accState.setClean(ACC_REG)
                    pc = afterInstr
                    continue
                }
                const fold = peekStoreFold(bytes, afterInstr, window.tos)
                accState.producer({ kind: "reg", reg: physReg(instr.target) })
                if(fold !== null) { accState.flush(e, fold.reg); pc = fold.afterNext; continue }
                pc = afterInstr
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
                    e.emit(arm.strSp(ACC_REG, window.spillOffset(instr.target)))
                    pc = afterInstr
                    continue
                }
                accState.flush(e, physReg(instr.target))
                pc = afterInstr
                continue

            case "CONST":
            {
                const fold = peekStoreFold(bytes, afterInstr, window.tos)
                const target = fold?.reg ?? ACC_REG
                if(arm.fitsImm8(instr.imm) && fold === null)
                {
                    accState.producer({ kind: "imm", value: instr.imm }) // stay pending — a later consumer may fold it
                    pc = afterInstr
                    continue
                }
                arm.synthesizeImm32(target, instr.imm).forEach(w => e.emit(w))
                accState.setClean(target)
                pc = fold?.afterNext ?? afterInstr
                continue
            }

            // `EXT` (byte ≥128) never reaches this switch at all —
            // `decodeInstr` itself throws first (§16 item 16 moved that
            // rejection from here into the decode step).

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
                        e.emit(arm.ldrSp(SCRATCH_REG, window.spillOffset(instr.target)))
                        operand = { kind: "reg", reg: SCRATCH_REG }
                    }
                }
                else if(instr.combo === "IMM_ACC") operand = { kind: "imm", value: instr.imm }
                else if(instr.combo === "POP_ACC") { operand = { kind: "reg", reg: window.topReg }; popAfter = true }
                else operand = undefined // PEEK_PEEK

                if(isComparisonOp(instr.op))
                {
                    // Fuse only when the *very* next instruction is the
                    // one thing that can actually consume a bare CMP as a
                    // condition (§10.1's zero-destination axis): a
                    // BR_TABLE selector (N ≤ 2 — N > 2 needs a real value,
                    // never a condition), or this LOOP's own condition
                    // sub-block closing. Anything else means this
                    // comparison is used as an ordinary value (design.md
                    // §16 item 8 — the RTL/VM already treat it as one;
                    // only this fusion-only assumption was the gap), so
                    // its 0/1 result has to be materialized instead.
                    const lookahead = afterInstr < bytes.length ? decodeInstr(bytes, afterInstr).instr : undefined
                    const fusesIntoBrTable = lookahead !== undefined && lookahead.op === "BR_TABLE" && lookahead.imm <= 2
                    const fusesIntoLoopExit = frame !== null && frame.kind === "loopCond" && lookahead !== undefined && lookahead.op === "BLOCK_END"
                    if(fusesIntoBrTable || fusesIntoLoopExit)
                    {
                        const trueCondition = emitComparison(e, accState, instr.op, operand)
                        if(popAfter) window.finishPop(e)
                        pendingComparisonCondition = trueCondition
                        pc = afterInstr
                        continue
                    }

                    const fold = peekStoreFold(bytes, afterInstr, window.tos)
                    const dest = fold?.reg ?? ACC_REG
                    materializeComparison(e, accState, instr.op, operand, dest)
                    if(popAfter) window.finishPop(e)
                    accState.setClean(dest)
                    pc = fold?.afterNext ?? afterInstr
                    continue
                }

                const clobbersAcc = combo === "REG_REG" || combo === "PEEK_PEEK"
                let dest: number
                // REG_REG writes back in place (isa-core.md §4.1 mode 2) —
                // out-of-window, that write-back target is memory, not a
                // register: compute into SCRATCH_REG (already holding the
                // operand read above) and store it back explicitly.
                let storeBackOffset: number | null = null
                let afterFold: number | null = null
                if(instr.combo === "REG_REG")
                {
                    if(inWindow(window.tos, instr.target)) dest = physReg(instr.target)
                    else { dest = SCRATCH_REG; storeBackOffset = window.spillOffset(instr.target) }
                }
                else if(instr.combo === "PEEK_PEEK") dest = window.topReg
                else
                {
                    const fold = peekStoreFold(bytes, afterInstr, window.tos)
                    dest = fold?.reg ?? ACC_REG
                    afterFold = fold?.afterNext ?? null
                }

                emitBinary(e, accState, instr.op, combo, operand, dest, clobbersAcc)
                if(storeBackOffset !== null) e.emit(arm.strSp(dest, storeBackOffset))
                if(popAfter) window.finishPop(e)
                pc = afterFold ?? afterInstr
                continue
            }
            }
        }

        if(frame !== null) throw new Error(`translateProc: procedure body ended with an open block`)
    }

    translateBody(null, 0)

    // BR_TABLE N>2's shared helper (blocks.ts's own header) — dead code
    // from a sequential-execution standpoint, reached only by the local
    // `BL`s `openBrTableJump` already placed; emitted once, here, only if
    // this body actually used it.
    if(brTableHelperSites.length > 0)
    {
        const helperOffset = emitBrTableHelper(e)
        for(const site of brTableHelperSites) e.patchBL(site, helperOffset)
    }

    // unaryops.ts's own CLZ/REVBITS software helpers — same "shared,
    // emitted once, only if actually used" shape as BR_TABLE N>2's above.
    if(unaryHelperSites.clz.length > 0)
    {
        const helperOffset = emitClzHelper(e)
        for(const site of unaryHelperSites.clz) e.patchBL(site, helperOffset)
    }
    if(unaryHelperSites.revbits.length > 0)
    {
        const helperOffset = emitRevbitsHelper(e)
        for(const site of unaryHelperSites.revbits) e.patchBL(site, helperOffset)
    }

    return { code: e.toUint16Array(), callSites }
}
