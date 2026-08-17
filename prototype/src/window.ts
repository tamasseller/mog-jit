/**
 * @ppl/jit-armv6m-prototype — the register window (docs/jit-armv6m.md §5)
 *
 * Pure `tos`-and-`k` math, plus the spill/fill emission it drives. Doesn't
 * know about the acc fusion state machine (accstate.ts) or block structure
 * (blocks.ts) — just "where does frame-relative slot k live right now, and
 * what native code does moving `tos` emit."
 *
 * `tos` here is this codebase's own convention (validate.ts/vm.ts): the
 * *count* of live slots (starts at `argCount`, `regs[tos++] = v` on push) —
 * one past the top slot's own index.
 *
 * **`sp` genuinely tracks current spilled depth — no fixed per-procedure
 * reservation.** Earlier revisions of this file reserved `localPeak * 4`
 * bytes once, in the procedure's own prologue, and addressed every spill
 * at a fixed offset from that unmoving base. isa-core.md §8.3 rules that
 * out: the whole-program bound it computes is a *maximum* over call
 * sites and local peaks, not a *sum* — provably tighter than reserving a
 * fixed block per procedure and stacking them additively down a call
 * chain, and only *achievable* if the underlying storage is actually
 * reused as frames come and go. So every ordinary spill here is a real,
 * `sp`-decrementing single-register `PUSH`; every ordinary fill is a real
 * `sp`-incrementing single-register `POP`. Spills always happen in
 * strictly ascending-`k` order as `tos` grows past the window, so the
 * *most recently* spilled slot is always the one closest to the current
 * `sp` — "natural order." One procedure's own spilled locals sit strictly
 * *below* whatever its caller had already spilled (caller's own `sp` only
 * ever decreases further, never gets reused out from under it), which is
 * what makes frames nest correctly with zero cross-procedure bookkeeping —
 * the context-free property §4 already claims for register-window
 * addressing, now also true of the spill stack itself.
 *
 * **`physReg`'s cyclic direction is chosen, not arbitrary — it's what makes
 * historical reloads batchable at all.** A batched `PUSH{list}`/`POP{list}`
 * is exactly equivalent to issuing that list as separate single-register
 * instructions in one specific order: **descending register number for
 * `PUSH`** (hardware puts the lowest register closest to the final `sp`,
 * i.e. it must have been written last), **ascending register number for
 * `POP`** (hardware reads the lowest register from the closest address,
 * i.e. it's read first). Spills always happen in ascending-`k` chronological
 * order (previous paragraph); `physReg` maps ascending `k` to *descending*
 * register number, so that real, already-emitted, non-adjacent spill
 * sequence is exactly equivalent to one hypothetical batched `PUSH` having
 * produced it — which means a batched `POP` (its exact, self-consistent
 * inverse) can read it back later, register-for-register, with no
 * reordering trick needed within a contiguous run.
 *
 * **Multi-register `PUSH`/`POP` batching — three consumers sharing one
 * `windowRuns` building block, not three separate tricks.** Thumb's
 * `PUSH`/`POP` register-list is an arbitrary 8-bit mask — *not* required
 * to be contiguous:
 *
 * 1. **Same registers in, same registers out ("mirrored").** If nothing
 *    touches a set of registers between a `PUSH{that set}` and a later
 *    `POP{that exact same set}`, the `POP` restores exactly what was
 *    pushed — trivially, by hardware's own inverse guarantee, regardless
 *    of `k`/wrap/ordering considerations entirely. This is what
 *    `spillForCall` uses for the caller's own non-argument locals (if any
 *    are resident alongside the args): one `PUSH` of the whole leftover
 *    bitmask, and later one mirrored `POP` of that same mask in
 *    `reloadAfterCall` — no per-`k` reasoning needed, because it was never
 *    a "reload keyed by `k`" in the first place, just an untouched
 *    round-trip forced by the callee's execution genuinely intervening
 *    (unlike `restoreWindow`'s block-exit case — see below).
 * 2. **Fresh values, remapped (`pushLargestKClosest`/`fillCalleeArgs`).**
 *    `CALL`'s args need to land in the *callee's* canonical `physReg(0)..`,
 *    not wherever they happened to live in the caller's own window — a real
 *    remap of currently-live values, so "mirrored" doesn't apply, but
 *    nothing pre-existing constrains the layout either: `pushLargestKClosest`
 *    splits the range with `windowRuns` and pushes the runs *forward*
 *    (pre-wrap first, post-wrap second) so the largest `k` — the smallest
 *    register, per `physReg`'s reversed direction — ends up closest to
 *    `sp`, exactly where `fillCalleeArgs`'s one ascending batched `POP`
 *    into `physReg(0)..` expects it.
 * 3. **Historical spilled data, read back (`popRuns`).** `restoreWindow`
 *    (ordinary `BLOCK_END`/`LOOP` truncation) and `reloadAfterCall`'s own
 *    deeper tail both need to reload data whose physical layout is already
 *    fixed by real chronological spills, not freely chosen — but per the
 *    identity above, that layout already matches a hypothetical batched
 *    `PUSH`, so `popRuns` splits the range with the same `windowRuns` and
 *    pops the runs in *reverse* (larger-`k`, closer-to-`sp` run first) —
 *    the mirror image of `pushLargestKClosest`'s iteration order, same
 *    building block either way.
 *
 * `restoreWindow` also has a no-op case neither trick above needs: nothing
 * intervenes between a truncation point and whatever reads the window next,
 * so any currently-resident, still-live-in-the-target register needs no
 * push *or* pop at all (`physReg(k)` doesn't depend on `tos`, so it's
 * already exactly where it needs to be) — only `k`s below the currently-
 * resident window are genuinely historical and go through `popRuns`.
 */

import {Emitter} from "./emit"
import * as arm from "./armv6"
import {WINDOW_BASE, WINDOW_SIZE} from "./registers"
import type {AccState} from "./accstate"

export function inWindow(tos: number, k: number): boolean
{
    return tos - k <= WINDOW_SIZE
}

/** Physical register holding frame-relative slot `k`, valid only when
 *  `inWindow(tos, k)` — a pure function of `k` alone, by design (§5): two
 *  control-flow paths that reconverge at the same `tos` always agree on
 *  every live slot's physical register with no reconciliation.
 *
 *  Deliberately *descending*-in-`k` (`k=0` at the top register, `r7`, not
 *  the bottom): `pushValue` always evicts registers in ascending-`k`
 *  chronological order, so this makes the real, already-emitted spill
 *  sequence for any run of consecutive `k`s exactly equivalent to one
 *  hypothetical batched `PUSH` having happened (batched `PUSH`'s own fixed
 *  ascending-register/ascending-address rule behaves like separate pushes
 *  issued in *descending* register order — see this file's header) —
 *  which is what lets `restoreWindow`/`reloadAfterCall` reload that data
 *  with a batched `POP` later, instead of one `POP` per register. */
export function physReg(k: number): number
{
    return WINDOW_BASE + (WINDOW_SIZE - 1 - (k % WINDOW_SIZE))
}

/** How many slots are currently spilled at a given `tos`. */
function spilledCount(tos: number): number
{
    return Math.max(0, tos - WINDOW_SIZE)
}

/** Byte offset from the *current* `sp` for slot `k`, valid only when `k` is
 *  genuinely spilled (`!inWindow(tos,k)`) — "most recently spilled closest
 *  to sp" (this file's header) means `k`'s distance from `sp`, in words, is
 *  exactly how many slots spilled *after* it are still resident on the
 *  stack. General — not `CALL`-specific: any local that falls out of the
 *  window (e.g. a procedure with more than `WINDOW_SIZE` concurrently-live
 *  locals, no `CALL` involved at all) needs this same addressing for its
 *  own `LOAD`/`STORE`, exactly as much as a callee whose `argCount` puts
 *  some of its own arguments below the window from the very first
 *  instruction (translateProc.ts's `LOAD`/`STORE` cases). */
export function spillOffset(tos: number, k: number): number
{
    return 4 * (spilledCount(tos) - 1 - k)
}

/** The (unordered) register set holding `k = bottom .. bottom+count-1` —
 *  a valid `PUSH`/`POP` mask regardless of wrap, since the mask itself
 *  doesn't care what order its members are listed in. */
function regsFor(bottom: number, count: number): number[]
{
    return Array.from({length: count}, (_, i) => physReg(bottom + i))
}

/**
 * The registers holding `k = bottom .. bottom+count-1` (`count ≤
 * WINDOW_SIZE`), split at the point (if any) where `physReg` wraps from
 * `r4` back to `r7` — at most two contiguous, k-ascending-but-register-
 * descending runs, each a valid single `PUSH`/`POP` register-list, in
 * k-ascending order: `[preWrap]` or `[preWrap, postWrap]`. Two consumers,
 * opposite iteration order: `pushLargestKClosest` (fresh call-arg values,
 * choosing a layout) walks these forward; `popRuns` (historical spilled
 * data, reading an already-fixed layout back) walks them in reverse — see
 * this file's header.
 */
function windowRuns(bottom: number, count: number): number[][]
{
    if(count === 0) return []
    const phase = bottom % WINDOW_SIZE
    const preWrapLen = Math.min(count, WINDOW_SIZE - phase)
    const preWrap = Array.from({length: preWrapLen}, (_, i) => physReg(bottom + i))
    const postWrapLen = count - preWrapLen
    if(postWrapLen === 0) return [preWrap]
    const postWrap = Array.from({length: postWrapLen}, (_, i) => physReg(bottom + preWrapLen + i))
    return [preWrap, postWrap]
}

/**
 * Push `k = bottom .. bottom+count-1` such that the *largest* `k` ends
 * up closest to the resulting `sp` (one or two `PUSH`es) — the one
 * ordering hardware `PUSH` can actually produce for a batched, wrapped
 * range: push the pre-wrap run (the *smaller* k's, including `bottom`
 * itself) first, the post-wrap run (the *larger* k's) second, since
 * whichever `PUSH` executes second lands at the lower address. (`physReg`'s
 * own reversed cyclic direction is what makes "largest k" — not
 * "smallest" — the end `fillCalleeArgs`'s target window needs closest;
 * see this file's header.) Only ever correct to use where the consumer
 * wants exactly this order — `fillCalleeArgs`'s own immediate, remapping
 * `POP`, not a same-register restore.
 */
function pushLargestKClosest(e: Emitter, bottom: number, count: number): void
{
    const runs = windowRuns(bottom, count) // [preWrap] or [preWrap, postWrap]
    for(let i = 0; i < runs.length; i++) e.emit(arm.push(runs[i]!))
}

/**
 * Pop `k = bottom .. bottom+count-1` — genuinely historical spilled data,
 * not fresh values — via at most two batched `POP`s instead of one per
 * slot: split with `windowRuns`, then consume the runs in reverse (the
 * larger-`k`, closer-to-`sp` run first), mirroring the natural LIFO reload
 * order. Correct *because* of `physReg`'s reversed cyclic direction: the
 * real, already-emitted spill sequence for any such run is exactly
 * equivalent to one hypothetical batched `PUSH` having produced it, so a
 * batched `POP` is its exact, self-consistent inverse (this file's header).
 */
function popRuns(e: Emitter, bottom: number, count: number): void
{
    const runs = windowRuns(bottom, count)
    for(let i = runs.length - 1; i >= 0; i--) e.emit(arm.pop(runs[i]!))
}

/** Window state for one procedure's translation — just the `tos` counter
 *  (docs/jit-armv6m.md §5's "counters for stack state"). */
export class Window
{
    tos: number

    constructor(argCount: number)
    {
        this.tos = argCount
    }

    /** The current top slot's physical register — the one thing every
     *  pop-shaped read (a bare `POP`, a `POP_ACC` combo operand, a
     *  `PEEK_PEEK` operand/destination) needs before deciding what to do
     *  with it. Doesn't itself touch `tos` or emit anything. */
    get topReg(): number
    {
        return physReg(this.tos - 1)
    }

    private get pushEvicts(): boolean {return this.tos >= WINDOW_SIZE}
    private get evictedByPush(): number {return this.tos - WINDOW_SIZE}
    private get popUncovers(): boolean {return this.tos - 1 >= WINDOW_SIZE}
    private get uncoveredByPop(): number {return this.tos - 1 - WINDOW_SIZE}

    /**
     * Push `accState`'s current value onto the window: spill whatever it's
     * about to evict (a real single-register `PUSH` — always exactly the
     * slot about to become the *most recently* spilled one, so it always
     * lands at the fresh top of the spill stack, no address computation
     * needed), materialize the value into its new home (`physReg(tos)`),
     * then bump `tos`.
     *
     * §10.1's "rotation eviction" hazard — a fused value living only in a
     * window register getting silently destroyed by the very rotation
     * this function performs — turns out not to need a rescue instruction
     * here, once actually worked through (docs/jit-armv6m.md §16 item 6):
     * `physReg(evictedByPush) === physReg(this.tos)` always (both reduce to
     * the same `k mod WINDOW_SIZE`), so *if* `accState` currently depends
     * on exactly that register, the value about to be pushed and the value
     * about to be evicted are provably the same value — the only way
     * `accState` could have come to depend on `physReg(evictedByPush)` in
     * the first place is a direct reference to slot `evictedByPush` itself
     * (nothing else currently maps to that register), and PUSH's own
     * semantics push whatever `accState` currently holds. The spill below
     * captures that value for the evicted slot's own record; the flush
     * that follows is then a same-register self-move, which
     * `materializeShape` already elides — no separate case needed.
     */
    pushValue(e: Emitter, accState: AccState): void
    {
        if(this.pushEvicts) e.emit(arm.push([physReg(this.evictedByPush)]))
        accState.flush(e, physReg(this.tos))
        this.tos += 1
    }

    /**
     * Complete a pop whose value was already read out of `topReg` — the
     * fill (if any, a real single-register `POP` — always exactly what's
     * currently closest to `sp`) and the `tos` decrement, together. Must
     * be called only once every read of `topReg` has already been
     * emitted, since the fill overwrites that exact register.
     */
    finishPop(e: Emitter): void
    {
        if(this.popUncovers)
            e.emit(arm.pop([physReg(this.uncoveredByPop)]))
        this.tos -= 1
    }
}

/**
 * §5's "block-exit truncation is not free": at `BLOCK_END`/a `LOOP`
 * back-edge, any TOS surplus above the target depth is *implicitly*
 * dropped (isa-core.md §8.1) — no bytecode-level pop sequence runs, so
 * `sp` needs rebalancing here regardless of whether any physical register
 * needs reloading. Nothing intervenes between a truncation point and
 * whatever reads the window next (unlike `CALL` — this file's header), so
 * any currently-resident register that's *also* still needed by the
 * target window needs no touching at all: `physReg(k)` doesn't depend on
 * `tos`, so it's already exactly where it needs to be. What's spilled
 * *above* the target window's own ceiling is abandoned outright (a bare
 * `sp` adjustment — nothing can still read it, isa-core.md §8.1), and
 * what's spilled at or below it — genuinely historical, natural-order
 * data — gets reloaded via `popRuns` (at most two batched `POP`s, per
 * `physReg`'s own reversed cyclic direction — this file's header), the
 * same larger-`k`-first order it was spilled in. Mutates `window.tos` to
 * `targetTos` directly.
 */
export function restoreWindow(e: Emitter, window: Window, targetTos: number): void
{
    // Number of ISA regs currently living physically on the stack.
    const spilledNow = spilledCount(window.tos)

    // Target number of ISA regs that should live on the physical stack.
    const spilledTarget = spilledCount(targetTos)

    // The stack index to be set **before** popping the regs that currently 
    // live on stack but need to be reloaded into registers
    const reloadTop = Math.min(spilledNow, targetTos) // exclusive; k ≥ this was never spilled at all

    // Move SP if needed for reloadTop enforcement
    if(spilledNow > reloadTop)
    {
        e.emit(arm.incrSp(4 * (spilledNow - reloadTop)))
    } 

    // Pop the regs that are being filled from stack, at most two batched POPs.
    popRuns(e, spilledTarget, reloadTop - spilledTarget)

    // All consistent, update bookkeping.
    window.tos = targetTos
}

/**
 * A terminator (`RETURN`/`TRAP`) needs `sp` rebalanced back to this
 * procedure's own entry depth before its epilogue runs — but unlike
 * `restoreWindow`, nothing downstream ever reads r4-r7 again, so there's
 * nothing to reload for: one bare `sp` adjustment undoes every spill this
 * procedure's own body ever made, in one instruction, regardless of how
 * deep `tos` got.
 */
export function discardWindow(e: Emitter, window: Window): void
{
    const spilled = spilledCount(window.tos)
    if(spilled > 0) e.emit(arm.incrSp(4 * spilled))
}

/**
 * `CALL`'s own shuffle (docs/jit-armv6m.md §6), first half. `stackArgs`
 * (`S`) splits the currently-resident window (`w = min(window.tos,
 * WINDOW_SIZE)` slots) into the caller's own non-argument locals, if any
 * (the bottom `w - S`), and the args themselves (the top `S`) — treated
 * differently (this file's header): the locals get one `PUSH` of the
 * whole leftover bitmask, unconditionally correct however it wraps, since
 * `reloadAfterCall` mirrors it back exactly; the args get the
 * remapping-aware `pushLargestKClosest`, since they're about to be
 * popped into the *callee's* canonical registers, not their own. Doesn't
 * move `window.tos`.
 */
export function spillForCall(e: Emitter, window: Window, stackArgs: number): void
{
    const w = Math.min(window.tos, WINDOW_SIZE)
    const m = Math.min(stackArgs, w)
    const bottom = window.tos - w
    const base = window.tos - m

    if(base > bottom) e.emit(arm.push(regsFor(bottom, base - bottom)))
    pushLargestKClosest(e, base, m)
}

/**
 * `CALL`'s shuffle, second half: fill the callee's own canonical phase-0
 * window — `physReg(stackArgs - m)..physReg(stackArgs - 1)`, the top `m =
 * min(stackArgs, WINDOW_SIZE - 1)` args — by reading back what
 * `spillForCall` just pushed via `pushLargestKClosest` (plus, when `m <
 * stackArgs`, whatever's genuinely deeper, spilled long before this call).
 * Capped at `WINDOW_SIZE - 1`, not `WINDOW_SIZE`: the callee's own last
 * argument (`argCount - 1`, delivered via `acc`, not through this function
 * at all) always lands at `physReg(argCount - 1)`, which is the *same*
 * physical register as `physReg(0)` whenever `stackArgs === WINDOW_SIZE`
 * exactly (`physReg` is periodic mod `WINDOW_SIZE`) — popping a full
 * `WINDOW_SIZE` args here would have this function's own arg 0 clobbered
 * the instant the callee's prologue writes its acc-delivered argument.
 *
 * Deliberately *not* one plain `pop(regsFor(...))` the way an earlier
 * version of this function did: that's only a single, un-wrapped register
 * run when the args being filled start at phase 0 (`stackArgs ≤ WINDOW_SIZE
 * - 1`, i.e. `m === stackArgs`) — once genuinely deep args exist (`m <
 * stackArgs`), the range `stackArgs - m .. stackArgs - 1` is phase-shifted
 * and can wrap `physReg`'s own cyclic boundary, and a *combined* pop across
 * that wrap silently reassigns which value lands in which register (traced
 * concretely while building `test/deep-args.test.ts`: `physReg(3)` came out
 * holding arg 5's value, not arg 3's). `popRuns` — the same "at most two
 * batched `POP`s, larger-`k`-first" mechanism `restoreWindow`/
 * `reloadAfterCall` already use for historical spilled data — handles the
 * wrap correctly, because the ordering it produces is exactly the ordering
 * `pushLargestKClosest` (this batch) and ordinary chronological spilling
 * (anything deeper) both already committed to on the real stack.
 *
 * No upper bound on `stackArgs` itself: whatever's left over (`stackArgs -
 * m`, if any) simply stays on the real stack, below what this function
 * pops — exactly where the callee's own out-of-window `LOAD`/`STORE`
 * (translateProc.ts, `spillOffset` above) expects to find it. `spillForCall`
 * needs no matching change: its own cap (`w = min(window.tos, WINDOW_SIZE)`)
 * already treats anything beyond the caller's own currently-resident window
 * as untouched, chronologically spilled long before this call — which is
 * precisely the same storage a deep callee argument needs to already be
 * sitting in.
 */
export function fillCalleeArgs(e: Emitter, stackArgs: number): void
{
    const m = Math.min(stackArgs, WINDOW_SIZE - 1)
    if(m === 0) return
    popRuns(e, stackArgs - m, m)
}

/**
 * `CALL`'s shuffle, final step: once the callee returns, r4-r7 hold
 * whatever *it* last left there — nothing to do with the caller's own
 * frame, not even the leftover locals `spillForCall` pushed (the
 * callee's execution is exactly the thing that forces a real round-trip
 * here, unlike `restoreWindow`'s block-exit case — see this file's
 * header). The leftover bitmask (`k = bottom .. targetTos-1`, where
 * `bottom` is `spillForCall`'s own — recomputed here from the *original*
 * `window.tos`, still valid since nothing has mutated it yet) comes back
 * with one mirrored `POP` of that exact same mask, regardless of wrap.
 * Anything deeper than `bottom` (spilled long before this call, via
 * ordinary natural-order spills, with no single `PUSH` to mirror) is
 * reloaded via `popRuns` — the same batched, larger-`k`-first way
 * `restoreWindow` uses. Together these always total exactly
 * `min(targetTos, WINDOW_SIZE)` registers restored — never more, since
 * `spillForCall` + `fillCalleeArgs` already left the actual spilled depth
 * at exactly `targetTos` (§6's own derivation).
 */
export function reloadAfterCall(e: Emitter, window: Window, targetTos: number): void
{
    const w = Math.min(window.tos, WINDOW_SIZE)
    const bottom = window.tos - w
    const count = Math.min(targetTos, WINDOW_SIZE)
    const deeperFloor = targetTos - count

    if(targetTos > bottom) e.emit(arm.pop(regsFor(bottom, targetTos - bottom)))

    // `bottom` alone assumes everything below it is the caller's own older
    // data, untouched by this call — true whenever `stackArgs ≤ w` (every
    // existing test before deep-args.test.ts), but not once `stackArgs`
    // exceeds the caller's own currently-resident window: some of what's
    // "below bottom" was *also* consumed as an argument (spilled there by
    // ordinary chronological pushes long before this call, same as any
    // other historical data, but still logically gone post-call, not
    // surviving caller state) — capped at `targetTos` so nothing at or
    // above it (still part of the arg range) gets reloaded at all.
    const historicalTop = Math.min(bottom, targetTos)
    popRuns(e, deeperFloor, historicalTop - deeperFloor)

    window.tos = targetTos
}
