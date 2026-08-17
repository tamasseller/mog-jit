/**
 * @ppl/jit-armv6m-prototype — the acc fusion state machine (docs/jit-armv6m.md §10.1)
 *
 * `acc`'s status at any point in the forward walk is exactly one of:
 *   - CLEAN(reg)   — already committed to a physical register (usually r0).
 *   - PENDING(shape) — not yet emitted; `shape` is `Imm(k)` or `Reg(r)`.
 *   - POISONED — a write-back-in-place combo (REG_REG/PEEK_PEEK) just ran;
 *     acc is clobbered (isa-core.md convention, rtl.ts's `COMBO` table) and
 *     nothing downstream may legitimately read it. Reading it anyway is a
 *     translator or input-program bug, not something to paper over —
 *     `peek`/`flush` throw rather than silently return garbage.
 *
 * `AccState` owns exactly that one slot of state and the mechanical
 * operations on it (`producer`, `flush`, `peek`, `poison`) — nothing here
 * knows a single Thumb opcode. *Which* native instruction a given op/combo
 * needs is binops.ts's entirely separate concern; `emitBinary` below is
 * just the one-line bridge between the two — read the current shape, hand
 * it to binops.ts, then record the resulting state (CLEAN or POISONED, per
 * whether `combo` clobbers acc). translateProc.ts decides *when* each of
 * these fires, per the transition table in §10.1's own doc comment.
 */

import { Emitter } from "./emit"
import { Shape, materializeShape } from "./shape"
import { ACC_REG } from "./registers"
import { emitBinaryOp } from "./binops"
import type { BinaryOpcode, ComboName } from "@ppl/machine"

type StateValue =
    | { kind: "clean"; reg: number }
    | { kind: "pending"; shape: Shape }
    | { kind: "poisoned" }

export class AccState
{
    private state: StateValue = { kind: "clean", reg: ACC_REG }

    /** A producer (CONST/LOAD/POP) just ran — defer materializing it. */
    producer(shape: Shape): void
    {
        this.state = { kind: "pending", shape }
    }

    /** Read the current value as a foldable operand, without discharging
     *  it — a run of consumers may all read the same still-pending value
     *  (§10.1's run-length argument). */
    peek(): Shape
    {
        if(this.state.kind === "poisoned")
            throw new Error("accstate: read of acc after a write-back-in-place combo clobbered it (docs/jit-armv6m.md §10.1's acc-clobbering convention)")
        
        return this.state.kind === "pending" ? this.state.shape : { kind: "reg", reg: this.state.reg }
    }

    /** Force materialization into `dstReg` (the "flush" transition) —
     *  used both for the no-match fallback and to seed a fresh CLEAN state
     *  after a real producer+consumer pair has run. */
    flush(e: Emitter, dstReg: number): void
    {
        materializeShape(e, this.peek(), dstReg) // throws if poisoned, via peek()
        this.state = { kind: "clean", reg: dstReg }
    }

    setClean(reg: number): void
    {
        this.state = { kind: "clean", reg }
    }

    /** REG_REG/PEEK_PEEK just clobbered acc — nothing downstream may read
     *  it until a new producer supersedes this state. */
    poison(): void
    {
        this.state = { kind: "poisoned" }
    }

    /** `flush`, but safe at a control-flow *merge* point (a `case`
     *  boundary — blocks.ts's `closeBlockEnd`) where `POISONED` isn't an
     *  error: a no-op there, since the acc-clobbering convention already
     *  forbids anything downstream from reading it regardless of which
     *  path arrived. Merge points matter because this state is one
     *  linear, compile-time-sequential belief — a value left `PENDING`
     *  at the end of one case (never itself read again *within* that
     *  case) would otherwise silently survive to be overwritten by the
     *  next case's own translation, so whatever the merged code reads
     *  afterward ends up being the *last* case's value, not whichever
     *  case actually ran at runtime. Flushing every case unconditionally
     *  before it merges is what keeps that from happening — cheap when
     *  already `CLEAN(dstReg)` (materializeShape's own no-op), the real
     *  cost paid only when a case actually left something pending. */
    flushLive(e: Emitter, dstReg: number): void
    {
        if(this.state.kind !== "poisoned") this.flush(e, dstReg)
    }
}

/**
 * Emit one arithmetic binary op and update `accState` to match. `operand`
 * is `combo`'s own right-hand side — a window register for
 * REG_ACC/REG_REG/POP_ACC, an immediate for IMM_ACC, or `undefined` for
 * PEEK_PEEK. `dest` says where the result must end up: `ACC_REG` or a
 * destination-fold target — translateProc.ts's one-token peek already
 * decided which. `clobbersAcc` is true exactly for REG_REG/PEEK_PEEK
 * (isa-core.md's own convention — see this file's header); the caller
 * derives it from `combo`, not from anything this function inspects.
 */
export function emitBinary(
    e: Emitter,
    accState: AccState,
    op: BinaryOpcode,
    combo: ComboName,
    operand: Shape | undefined,
    dest: number,
    clobbersAcc: boolean,
): void
{
    emitBinaryOp(e, op, combo, accState.peek(), operand, dest)
    if(clobbersAcc) accState.poison()
    else accState.setClean(dest)
}
