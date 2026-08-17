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
 * **Multi-register `PUSH`/`POP` batching (`CALL`'s shuffle only) — two
 * different tricks for two different consumers, not one.** Thumb's
 * `PUSH`/`POP` register-list is an arbitrary 8-bit mask — *not* required
 * to be contiguous — so it has two genuinely different uses here:
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
 * 2. **Different registers out than in (remapping).** `CALL`'s args need
 *    to land in the *callee's* canonical `physReg(0)..`, not wherever they
 *    happened to live in the caller's own window — a real remap, so
 *    "mirrored" doesn't apply. Hardware ascending-register-order still
 *    constrains what's achievable here: a batched `PUSH` can only
 *    reliably deliver "smallest `k` ends up closest to `sp`" (push the
 *    post-wrap/larger-`k` run first, pre-wrap/smaller-`k` run second, so
 *    whichever executes second — landing lower — is `arg0`). That's
 *    exactly what `fillCalleeArgs`'s one combined ascending-register `POP`
 *    into `physReg(0)..` wants, so `pushSmallestKClosest` is only ever
 *    used for the args, never for a same-register restore.
 *
 * `restoreWindow` (ordinary `BLOCK_END`/`LOOP` truncation) needs neither
 * trick: nothing intervenes between a truncation point and whatever reads
 * the window next, so any currently-resident, still-live-in-the-target
 * register needs no push *or* pop at all (`physReg(k)` doesn't depend on
 * `tos`, so it's already exactly where it needs to be) — only genuinely
 * historical, individually-and-natural-order-spilled data (from `k`s below
 * the currently-resident window) ever needs an actual reload, and that has
 * no single `PUSH` to mirror, so it stays individual, descending-`k`.
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
 *  every live slot's physical register with no reconciliation. */
export function physReg(k: number): number
{
    return WINDOW_BASE + (k % WINDOW_SIZE)
}

/** How many slots are currently spilled at a given `tos`. */
function spilledCount(tos: number): number
{
    return Math.max(0, tos - WINDOW_SIZE)
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
 * `r7` back to `r4` — at most two contiguous, ascending runs, each a
 * valid single `PUSH`/`POP` register-list, in k-ascending order:
 * `[preWrap]` or `[preWrap, postWrap]`. Only `pushSmallestKClosest` (the
 * args' own remap) needs this split; a same-register round-trip doesn't.
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
 * Push `k = bottom .. bottom+count-1` such that the *smallest* `k` ends
 * up closest to the resulting `sp` (one or two `PUSH`es) — the one
 * ordering hardware `PUSH` can actually produce for a batched, wrapped
 * range: push the post-wrap run (the *larger* k's) first, the pre-wrap
 * run (the *smaller* k's, including `bottom` itself) second, since
 * whichever `PUSH` executes second lands at the lower address. Only ever
 * correct to use where the consumer wants exactly this order —
 * `fillCalleeArgs`'s own immediate, remapping `POP`, not a same-register
 * restore (see this file's header).
 */
function pushSmallestKClosest(e: Emitter, bottom: number, count: number): void
{
    const runs = windowRuns(bottom, count) // [preWrap] or [preWrap, postWrap]
    for(let i = runs.length - 1; i >= 0; i--) e.emit(arm.push(runs[i]!))
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
     * then bump `tos`. Throws the rotation-eviction guard (this file's
     * header; §10.1's "essentially never fires" case) if the value about
     * to be evicted is exactly what `accState` still depends on.
     */
    pushValue(e: Emitter, accState: AccState): void
    {
        if(this.pushEvicts)
        {
            const dep = accState.dependsOnReg()
            if(dep !== null && dep === physReg(this.evictedByPush))
                throw new Error(`window: rotation-eviction fallback not implemented (docs/jit-armv6m.md §10.1, "essentially never fires" — this corpus apparently hit it)`)
            e.emit(arm.push([physReg(this.evictedByPush)]))
        }
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
 * what's spilled at or below it — genuinely historical, individually,
 * natural-order data, with no single `PUSH` to mirror — gets reloaded
 * individually, in descending-`k` order, the same order it was spilled
 * in. Mutates `window.tos` to `targetTos` directly.
 */
export function restoreWindow(e: Emitter, window: Window, targetTos: number): void
{
    const spilledNow = spilledCount(window.tos)
    const spilledTarget = spilledCount(targetTos)
    const reloadTop = Math.min(spilledNow, targetTos) // exclusive; k ≥ this was never spilled at all

    if(spilledNow > reloadTop) e.emit(arm.incrSp(4 * (spilledNow - reloadTop)))

    for(let k = reloadTop - 1; k >= spilledTarget; k--)
        e.emit(arm.pop([physReg(k)]))

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
 * remapping-aware `pushSmallestKClosest`, since they're about to be
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
    pushSmallestKClosest(e, base, m)
}

/**
 * `CALL`'s shuffle, second half: fill the callee's own canonical phase-0
 * window (`physReg(0)..physReg(stackArgs-1)`) by reading back the `m`
 * args `spillForCall` just pushed via `pushSmallestKClosest` — one plain
 * multi-register `POP`, valid because (for `stackArgs ≤ WINDOW_SIZE`,
 * this prototype's only supported case — see translateProc.ts's own
 * guard) the callee's own in-window range always starts at `physReg(0)`
 * exactly, and `pushSmallestKClosest` put the smallest arg (`arg0`)
 * closest to `sp`.
 */
export function fillCalleeArgs(e: Emitter, stackArgs: number): void
{
    const m = Math.min(stackArgs, WINDOW_SIZE)
    if(m === 0) return
    e.emit(arm.pop(regsFor(0, m)))
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
 * ordinary individual natural-order spills, with no single `PUSH` to
 * mirror) is reloaded the same individual, descending-`k` way
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

    for(let k = bottom - 1; k >= deeperFloor; k--)
        e.emit(arm.pop([physReg(k)]))

    window.tos = targetTos
}
