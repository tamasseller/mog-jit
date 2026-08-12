/**
 * @ppl/jit-armv6m-prototype — the acc fusion state machine (docs/jit-armv6m.md §10.1)
 *
 * `acc`'s status at any point in the forward walk is exactly one of:
 *   - CLEAN(reg)   — already committed to a physical register (usually r3).
 *   - PENDING(shape) — not yet emitted; `shape` is `Imm(k)` or `Reg(r)`.
 *   - POISONED — a write-back-in-place combo (REG_REG/PEEK_PEEK) just ran;
 *     acc is clobbered (isa-core.md convention, rtl.ts's `COMBO` table) and
 *     nothing downstream may legitimately read it. Reading it anyway is a
 *     translator or input-program bug, not something to paper over —
 *     `peek`/`flush` throw rather than silently return garbage.
 * `AccState` owns exactly that one slot of state and the mechanical
 * operations on it (`producer`, `flush`, `peek`) — it does not decide
 * *when* those fire; translateProc.ts's per-instruction loop does, per the
 * transition table in §10.1's doc comment.
 *
 * The per-opcode native-codegen table (`emitBinary`) lives here too, since
 * it only exists to serve the fusion decision: which operand shape can
 * fold into which op's native encoding is *the* thing §10.1's classification
 * table (reproduced below) is about — factoring it out into its own module
 * would just split one decision across two files.
 *
 * Scope: only the opcodes/combos the current test corpus (leb128_len +
 * the four core-testsuite algorithms) actually needs are implemented —
 * ADD (commutative3), SUB/RSUB (order-sensitive, mirror-in-spirit),
 * SHL/SHR/ASR with an immediate shift count (shiftImm), and AND/OR/XOR/MUL
 * plus any shift with a *register* count (twoOpInPlace, §10.1's bottom
 * row — Thumb-1's 2-operand encoding always reads the destination as an
 * input too, so pending state must flush before one of these without
 * exception). `PEEK_PEEK` for a twoOpInPlace op is not implemented — see
 * that branch below for why register-role assignment for a non-symmetric
 * op (shift) genuinely differs from a symmetric one (AND/OR/XOR/MUL) there,
 * and this corpus never exercises it either way. Comparisons are handled
 * entirely by blocks.ts's branch-fusion path, never through this table.
 */

import { Emitter } from "./emit"
import * as arm from "./armv6"
import type { BinaryOpcode, ComboName } from "@ppl/machine"

export const ACC_REG = 3 // r3 — this procedure's own committed-acc register.
/** Scratch register for materializing an immediate operand ahead of a
 *  2-op-in-place instruction that has no immediate form at all (§10.1's
 *  bottom row), or for the rare imm-OP-imm degenerate case elsewhere —
 *  never acc's own home, never a window register (window.ts's r4-r7). */
export const SCRATCH_REG = 2 // r2

export type Shape =
    | { kind: "imm"; value: number }
    | { kind: "reg"; reg: number }

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
        if(this.state.kind === "poisoned")
            throw new Error("accstate: flush of acc after a write-back-in-place combo clobbered it")
        if(this.state.kind === "pending")
        {
            const shape = this.state.shape
            if(shape.kind === "imm") arm.synthesizeImm32(dstReg, shape.value).forEach(w => e.emit(w))
            else if(shape.reg !== dstReg) e.emit(arm.movHi(dstReg, shape.reg))
        }
        else if(this.state.reg !== dstReg)
        {
            e.emit(arm.movHi(dstReg, this.state.reg))
        }
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

    /** The register this state currently depends on, if any — an Imm
     *  shape, a poisoned state, or CLEAN(r3) never depends on a window
     *  register, so this is `null` for those; a front-folded Reg shape or
     *  a destination-folded CLEAN(window reg) does. Used by the
     *  rotation-eviction guard (window.ts's doc comment; translateProc.ts's
     *  push sites). */
    dependsOnReg(): number | null
    {
        if(this.state.kind === "pending" && this.state.shape.kind === "reg") return this.state.shape.reg
        if(this.state.kind === "clean") return this.state.reg
        return null
    }
}

// ── §10.1's per-opcode classification, restricted to this corpus's needs ───

type BinOpKind = "addSubRsub" | "shiftImm" | "twoOpInPlace"

function classify(op: BinaryOpcode, combo: ComboName): BinOpKind
{
    if(op === "ADD" || op === "SUB" || op === "RSUB") return "addSubRsub"
    if((op === "SHL" || op === "SHR" || op === "ASR") && combo === "IMM_ACC") return "shiftImm"
    return "twoOpInPlace" // AND, OR, XOR, MUL, and any shift with a register count
}

function materializeToScratch(e: Emitter, value: number): number
{
    arm.synthesizeImm32(SCRATCH_REG, value).forEach(w => e.emit(w))
    return SCRATCH_REG
}

function shapeToReg(e: Emitter, shape: Shape): number
{
    return shape.kind === "reg" ? shape.reg : materializeToScratch(e, shape.value)
}

/** `Rd = n ± k` via whichever native form fits — `ADDIMM`/`SUBIMM` (3-op,
 *  imm3) when possible, the 2-op imm8 form when `dest === n` and `k` fits
 *  8 bits, else materialize `k` and fall back to the plain register form.
 *  Never fails to produce *something* correct — just not always the
 *  shortest encoding, exactly the "no match → flush" fallback §10.1
 *  describes, specialized to add/sub's own operand-width limits rather
 *  than to a whole missing native form. */
function emitAddOrSubImm(e: Emitter, sub: boolean, dest: number, n: number, k: number): void
{
    if(arm.fitsImm3(k)) { e.emit(sub ? arm.subsImm3(dest, n, k) : arm.addsImm3(dest, n, k)); return }
    if(arm.fitsImm8(k) && dest === n) { e.emit(sub ? arm.subsImm8(dest, k) : arm.addsImm8(dest, k)); return }
    const scratch = materializeToScratch(e, k)
    e.emit(sub ? arm.subsReg3(dest, n, scratch) : arm.addsReg3(dest, n, scratch))
}

/** `Rd = k - n` — immediate minus register, which has no direct native
 *  form (§10.1's own note: "SUB ... has no matching form ... must flush
 *  unless k happens to be 0") — `k===0` degenerates to `NEG`; otherwise
 *  materialize `k` and use the plain register-minus-register form. */
function emitRsubImmAsLeft(e: Emitter, dest: number, k: number, n: number): void
{
    if(k === 0) { e.emit(arm.negs(dest, n)); return }
    const scratch = materializeToScratch(e, k)
    e.emit(arm.subsReg3(dest, scratch, n))
}

/** ADD/SUB/RSUB, covering every (accShape, operandShape) combination —
 *  `operand === undefined` means PEEK_PEEK, whose right-hand operand is
 *  `dest` itself (isa-core.md §4.1: `[tos-1] = acc ⟨op⟩ [tos-1]`, read
 *  before being overwritten by this same instruction, safe since native
 *  3-op forms read all sources before writing `Rd`). */
function emitAddSubRsub(e: Emitter, op: "ADD" | "SUB" | "RSUB", dest: number, accShape: Shape, operand: Shape | undefined): void
{
    const rhs: Shape = operand ?? { kind: "reg", reg: dest }

    if(op === "ADD")
    {
        if(accShape.kind === "reg")
        {
            if(rhs.kind === "reg") e.emit(arm.addsReg3(dest, accShape.reg, rhs.reg))
            else emitAddOrSubImm(e, false, dest, accShape.reg, rhs.value)
        }
        else if(rhs.kind === "reg")
        {
            emitAddOrSubImm(e, false, dest, rhs.reg, accShape.value)
        }
        else
        {
            emitAddOrSubImm(e, false, dest, shapeToReg(e, accShape), rhs.value) // both imm — rare/degenerate
        }
        return
    }

    if(op === "SUB") // acc − rhs
    {
        if(accShape.kind === "reg")
        {
            if(rhs.kind === "reg") e.emit(arm.subsReg3(dest, accShape.reg, rhs.reg))
            else emitAddOrSubImm(e, true, dest, accShape.reg, rhs.value)
        }
        else if(rhs.kind === "reg")
        {
            emitRsubImmAsLeft(e, dest, accShape.value, rhs.reg)
        }
        else
        {
            emitAddOrSubImm(e, true, dest, shapeToReg(e, accShape), rhs.value) // both imm
        }
        return
    }

    // RSUB: rhs − acc
    if(rhs.kind === "reg")
    {
        if(accShape.kind === "reg") e.emit(arm.subsReg3(dest, rhs.reg, accShape.reg))
        // acc imm (k), rhs reg (n): dest = n − k — an ordinary register-
        // minus-immediate, the ADD/SUB row's own fold, not the
        // immediate-as-Rn case.
        else emitAddOrSubImm(e, true, dest, rhs.reg, accShape.value)
    }
    else if(accShape.kind === "reg")
    {
        emitRsubImmAsLeft(e, dest, rhs.value, accShape.reg)
    }
    else
    {
        const n = shapeToReg(e, accShape)
        emitRsubImmAsLeft(e, dest, rhs.value, n) // both imm
    }
}

function shiftOpImm(op: "SHL" | "SHR" | "ASR", d: number, m: number, amount: number): number
{
    if(op === "SHL") return arm.lslsImm(d, m, amount)
    if(op === "SHR") return arm.lsrsImm(d, m, amount)
    return arm.asrsImm(d, m, amount)
}

function twoOpInPlaceNative(op: BinaryOpcode, dn: number, m: number): number
{
    switch(op)
    {
        case "AND": return arm.ands(dn, m)
        case "OR": return arm.orrs(dn, m)
        case "XOR": return arm.eors(dn, m)
        case "MUL": return arm.muls(dn, m)
        case "SHL": return arm.lslsReg(dn, m)
        case "SHR": return arm.lsrsReg(dn, m)
        case "ASR": return arm.asrsReg(dn, m)
        default: throw new Error(`accstate: ${op} has no 2-op-in-place native form`)
    }
}

/**
 * Emit one arithmetic binary op. `operand` is `combo`'s own right-hand
 * side — a window register (or scratch, already materialized) for
 * REG_ACC/REG_REG/POP_ACC, an immediate for IMM_ACC, or `undefined` for
 * PEEK_PEEK (operand is `dest` itself). `acc`'s own current value comes
 * from `accState.peek()`, read internally.
 *
 * `dest` says where the result must end up: `ACC_REG` (the default) or a
 * specific window register (a destination-fold, from a following
 * STORE/write-back combo) — translateProc.ts's one-token peek already
 * decided which. `clobbersAcc` is true exactly for REG_REG/PEEK_PEEK
 * (isa-core.md's own convention — see this file's header); the caller
 * derives it from `combo`, not from anything this function can see.
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
    const kind = classify(op, combo)
    const accShape = accState.peek()

    if(kind === "twoOpInPlace")
    {
        if(operand === undefined)
            throw new Error(`accstate: PEEK_PEEK for ${op} (2-op-in-place) is not implemented — register-role assignment for a non-symmetric op genuinely differs from AND/OR/XOR/MUL's, and this corpus never exercises either shape (docs/jit-armv6m.md §10.1)`)
        accState.flush(e, ACC_REG) // §10.1's bottom row: never folds — flush first, unconditionally.
        const m = operand.kind === "reg" ? operand.reg : materializeToScratch(e, operand.value)
        e.emit(twoOpInPlaceNative(op, ACC_REG, m)) // ACC_REG = ACC_REG op m — alias-safe: m is never r3.
        if(dest !== ACC_REG) e.emit(arm.movHi(dest, ACC_REG))
    }
    else if(kind === "shiftImm")
    {
        if(operand === undefined || operand.kind !== "imm")
            throw new Error(`accstate: ${op} shiftImm classification requires an immediate operand`)
        e.emit(shiftOpImm(op as "SHL" | "SHR" | "ASR", dest, shapeToReg(e, accShape), operand.value))
    }
    else
    {
        emitAddSubRsub(e, op as "ADD" | "SUB" | "RSUB", dest, accShape, operand)
    }

    if(clobbersAcc) accState.poison()
    else accState.setClean(dest)
}
