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
 * one past the top slot's own index. docs/jit-armv6m.md §5 states
 * `in_window(k) ⟺ tos − k < 4`, which combined with *that* convention puts
 * only 3 slots in window at any given `tos` (e.g. tos=4, k=0: 4−0=4, not
 * <4 — excluded, leaving only k=1,2,3) — one short of "4-deep window,
 * r4-r7". Reconciled here as `tos − k <= 4`, which puts exactly
 * `{tos-4, ..., tos-1}` (4 slots) in window; `phys(k) = r4 + (k mod 4)` is
 * unchanged either way. Likely just two different "tos" indexing
 * conventions (count vs. top-slot-index) reading as the same formula on
 * paper — worth reconciling with the doc directly, but this is exactly the
 * kind of thing prototyping was for.
 *
 * A slot's spilled-out register and the value that later uncovers it share
 * the same physical register by construction: `evictedByPush = tos - 4`
 * and `tos` itself have the same residue mod 4, and likewise
 * `uncoveredByPop = (tos-1) - 4` and `tos-1` — subtracting the window size
 * never changes `k mod 4`. So push/pop spill and fill always target
 * exactly the register the new/popped value itself already occupies, never
 * a third one — which is *why* spill-then-write and read-then-fill are
 * always safe in that order (never a distinct-register race).
 */

import {Emitter} from "./emit"
import * as arm from "./armv6"

export const WINDOW_BASE = 4 // r4
export const WINDOW_SIZE = 4 // r4..r7

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

/** Byte offset of slot `k`'s spill-stack home, SP-relative — this
 *  procedure's own reserved SP region (translateProc.ts's prologue/
 *  epilogue carve it out; no shared global spill stack yet, since nothing
 *  here does CALL/eviction, so plain SP stands in for §2's dedicated
 *  spill-stack pointer for now). One word reserved per possible slot
 *  index, including ones that never actually leave the window — simple
 *  and correct, at the cost of a few wasted words. */
export function spillOffset(k: number): number
{
    return k * 4
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

    /** Would pushing one more slot right now evict a still-live slot from
     *  its physical register? */
    get pushEvicts(): boolean {return this.tos >= WINDOW_SIZE}

    /** Slot index a push-right-now would evict — valid only when `pushEvicts`;
     *  shares a physical register with the new top slot (`tos`) by construction. */
    get evictedByPush(): number {return this.tos - WINDOW_SIZE}

    /** Would popping the current top slot uncover a previously-spilled one? */
    get popUncovers(): boolean {return this.tos - 1 >= WINDOW_SIZE}

    /** Slot index uncovered by popping right now — valid only when
     *  `popUncovers`; shares a physical register with the popped top slot
     *  (`tos-1`) by construction, which is exactly why the fill must be
     *  emitted only *after* the popped value's own consumer has read that
     *  register. */
    get uncoveredByPop(): number {return this.tos - 1 - WINDOW_SIZE}

    /** Emit the spill for `evictedByPush`, if any. Call before writing the
     *  new value into `physReg(this.tos)` and before `push()`. */
    emitSpillIfNeeded(e: Emitter): void
    {
        if(this.pushEvicts)
            e.emit(arm.strSp(physReg(this.evictedByPush), spillOffset(this.evictedByPush)))
    }

    /** Emit the fill for `uncoveredByPop`, if any. Call after every
     *  consumer of the popped top slot's value has already read
     *  `physReg(this.tos-1)` — the fill overwrites that same register. */
    emitFillIfNeeded(e: Emitter): void
    {
        if(this.popUncovers)
            e.emit(arm.ldrSp(physReg(this.uncoveredByPop), spillOffset(this.uncoveredByPop)))
    }

    push(): void {this.tos += 1}
    pop(): void {this.tos -= 1}
}

/**
 * §5's "block-exit truncation is not free": at `BLOCK_END`/`RETURN`/a
 * `LOOP` back-edge, any TOS surplus above the target depth is *implicitly*
 * dropped (isa-core.md §8.1) — no bytecode-level pop sequence runs, so the
 * physical registers r4-r7 can be left holding values that belong to the
 * dropped slots rather than what `targetTos`'s own window expects. This
 * restores them: for every slot the *target* depth's window covers that
 * isn't already correctly resident at the *current* depth, reload it from
 * the spill stack. Unlike a single pop's fill (`emitFillIfNeeded`), a batch
 * restore never has the same-register read-then-overwrite ordering
 * constraint — the (at most `WINDOW_SIZE`) slots being restored all have
 * distinct `k mod 4` residues, so none of these loads can clobber a
 * register some other slot in the same batch still needs to read first.
 * Mutates `window.tos` to `targetTos` directly, bypassing `push`/`pop`
 * one-at-a-time (this *is* the "pop-multiple-equivalent" the doc
 * describes, not a loop of single pops).
 */
export function restoreWindow(e: Emitter, window: Window, targetTos: number): void
{
    const from = Math.max(0, targetTos - WINDOW_SIZE)
    
    for(let k = from; k < targetTos; k++)
        if(!inWindow(window.tos, k))
            e.emit(arm.ldrSp(physReg(k), spillOffset(k)))

    window.tos = targetTos
}
