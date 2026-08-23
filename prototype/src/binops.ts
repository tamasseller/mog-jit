/**
 * @ppl/jit-armv6m-prototype — per-opcode native codegen (docs/design.md §10.1)
 *
 * Pure instruction selection: given `acc`'s current `Shape`, the combo's
 * own right-hand operand, and a destination register, emit whichever
 * native Thumb form fits. Nothing here reads or writes accstate.ts's
 * CLEAN/PENDING/POISONED state — it only ever sees `Shape` values handed
 * in, and only ever returns by emitting instructions. That split is the
 * point: this file is "which operand shape folds into which op's native
 * encoding" (§10.1's classification table, reproduced in `classify`
 * below), full stop — it doesn't know or care *why* a Shape arrived in
 * whichever state it's in, which is accstate.ts's entire job instead.
 *
 * Covers ADD (commutative3), SUB/RSUB (order-sensitive, mirror-in-spirit),
 * SHL/SHR/ASR with an immediate shift count (shiftImm), and AND/OR/XOR/MUL
 * plus any shift with a *register* count (twoOpInPlace, §10.1's bottom
 * row — Thumb-1's 2-operand encoding always reads the destination as an
 * input too, so a pending value must be materialized before one of these
 * without exception) — including `PEEK_PEEK` (docs/design.md §16 item 11:
 * `dest` itself as the right-hand operand, same idiom `emitAddSubRsub`
 * already uses).
 */

import { Emitter } from "./emit"
import * as arm from "./armv6"
import { Shape, materializeShape, shapeToReg } from "./shape"
import { ACC_REG, SCRATCH_REG } from "./registers"
import type { BinaryOpcode, ComboName } from "@ppl/machine"

type BinOpKind = "addSubRsub" | "shiftImm" | "twoOpInPlace"

function classify(op: BinaryOpcode, combo: ComboName): BinOpKind
{
    if(op === "ADD" || op === "SUB" || op === "RSUB") return "addSubRsub"
    if((op === "SHL" || op === "SHR" || op === "ASR") && combo === "IMM_ACC") return "shiftImm"
    return "twoOpInPlace" // AND, OR, XOR, MUL, and any shift with a register count
}

/** `Rd = n ± k`, materializing `k` into scratch first when no native
 *  immediate form fits — `ADDIMM`/`SUBIMM` (3-op, imm3) when possible, the
 *  2-op imm8 form when `dest === n` and `k` fits 8 bits, else the plain
 *  register form. Never fails to produce *something* correct — just not
 *  always the shortest encoding: §10.1's own "no match → flush" fallback,
 *  scoped to add/sub's operand-width limits rather than a whole missing
 *  native form.
 *
 *  `n` isn't always a register `dest` is free to leave alone: a caller can
 *  pass `rhs.reg` straight through, and when that operand came from an
 *  out-of-window stack slot (translateProc.ts's own `ldrSp(SCRATCH_REG,
 *  ...)` reload), `n === SCRATCH_REG`. Materializing `k` into SCRATCH_REG
 *  right after would silently clobber that just-reloaded value before the
 *  final op ever reads it, computing `k op k` instead of `n op k` — copy
 *  `n` into `dest` first so it survives k's own materialization. */
function addOrSubWithImm(e: Emitter, sub: boolean, dest: number, n: number, k: number): void
{
    if(arm.fitsImm3(k)) { e.emit(sub ? arm.subsImm3(dest, n, k) : arm.addsImm3(dest, n, k)); return }
    if(arm.fitsImm8(k) && dest === n) { e.emit(sub ? arm.subsImm8(dest, k) : arm.addsImm8(dest, k)); return }
    if(n === SCRATCH_REG) { materializeShape(e, { kind: "reg", reg: n }, dest); n = dest }
    materializeShape(e, { kind: "imm", value: k }, SCRATCH_REG)
    e.emit(sub ? arm.subsReg3(dest, n, SCRATCH_REG) : arm.addsReg3(dest, n, SCRATCH_REG))
}

/** `Rd = k - n` — immediate minus register, which has no direct native
 *  form (§10.1's own note: "SUB ... has no matching form ... must flush
 *  unless k happens to be 0") — `k===0` degenerates to `NEG`; otherwise
 *  materialize `k` into scratch and use the plain register-minus-register
 *  form. Same `n === SCRATCH_REG` hazard as `addOrSubWithImm` above, same
 *  fix. */
function emitRsubImmAsLeft(e: Emitter, dest: number, k: number, n: number): void
{
    if(k === 0) { e.emit(arm.negs(dest, n)); return }
    if(n === SCRATCH_REG) { materializeShape(e, { kind: "reg", reg: n }, dest); n = dest }
    materializeShape(e, { kind: "imm", value: k }, SCRATCH_REG)
    e.emit(arm.subsReg3(dest, SCRATCH_REG, n))
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
            else addOrSubWithImm(e, false, dest, accShape.reg, rhs.value)
        }
        else if(rhs.kind === "reg")
        {
            addOrSubWithImm(e, false, dest, rhs.reg, accShape.value)
        }
        else
        {
            // both imm — rare/degenerate. Materialize accShape into `dest`
            // itself, not SCRATCH_REG: addOrSubWithImm's own fallback below
            // (when k doesn't fit imm3/imm8) materializes k into
            // SCRATCH_REG too, and dest is guaranteed distinct from
            // SCRATCH_REG at every call site that can reach this branch
            // (registers.ts's own invariant — SCRATCH_REG is "never a
            // window register, never acc's own home"), so this can't alias
            // the way `shapeToReg(e, accShape, SCRATCH_REG)` used to (that
            // returned SCRATCH_REG as `n`, which the fallback's own
            // materialize of `k` into the *same* SCRATCH_REG then silently
            // clobbered, computing `k op k` instead of `accShape.value op
            // k`).
            materializeShape(e, accShape, dest)
            addOrSubWithImm(e, false, dest, dest, rhs.value)
        }
        return
    }

    if(op === "SUB") // acc − rhs
    {
        if(accShape.kind === "reg")
        {
            if(rhs.kind === "reg") e.emit(arm.subsReg3(dest, accShape.reg, rhs.reg))
            else addOrSubWithImm(e, true, dest, accShape.reg, rhs.value)
        }
        else if(rhs.kind === "reg")
        {
            emitRsubImmAsLeft(e, dest, accShape.value, rhs.reg)
        }
        else
        {
            materializeShape(e, accShape, dest) // both imm — see ADD's own comment above
            addOrSubWithImm(e, true, dest, dest, rhs.value)
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
        else addOrSubWithImm(e, true, dest, rhs.reg, accShape.value)
    }
    else if(accShape.kind === "reg")
    {
        emitRsubImmAsLeft(e, dest, rhs.value, accShape.reg)
    }
    else
    {
        materializeShape(e, accShape, dest) // both imm — see ADD's own comment above
        emitRsubImmAsLeft(e, dest, rhs.value, dest)
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
        default: throw new Error(`binops: ${op} has no 2-op-in-place native form`)
    }
}

/**
 * Emit one arithmetic binary op. `accShape` is `acc`'s current value
 * (whatever accstate.ts's `peek()` returned — this function never asks how
 * it got there); `operand` is `combo`'s own right-hand side — a window
 * register for REG_ACC/REG_REG/POP_ACC, an immediate for IMM_ACC, or
 * `undefined` for PEEK_PEEK (operand is `dest` itself). `dest` says where
 * the result must end up: `ACC_REG` or a destination-fold target — the
 * caller (accstate.ts's `emitBinary`) already decided which, from the
 * one-token peek.
 */
export function emitBinaryOp(
    e: Emitter,
    op: BinaryOpcode,
    combo: ComboName,
    accShape: Shape,
    operand: Shape | undefined,
    dest: number,
): void
{
    const kind = classify(op, combo)

    if(kind === "twoOpInPlace")
    {
        // §10.1's bottom row: never folds — materialize acc into ACC_REG
        // specifically, unconditionally, even when `accShape` is already
        // some *other* register. That other register can be a window
        // register — a live variable's own home, front-folded in as an
        // operand but not otherwise being written here — and the native
        // 2-op-in-place form's `Rdn` slot is both read *and written*.
        // Computing directly into it (as `shapeToReg` would, skipping the
        // "already a register" case) would silently corrupt that
        // variable for good the moment `dest` is anything else. `m`
        // (`Rm`, read-only in this form) has no such hazard, so it alone
        // is safe to leave wherever it already is.
        //
        // `operand === undefined` means PEEK_PEEK — its right-hand side
        // is `dest` itself (`[tos-1] = acc ⟨op⟩ [tos-1]`), same idiom
        // `emitAddSubRsub` already established: `dest` (always a window
        // register here, never `ACC_REG`/`SCRATCH_REG`) is safe to read
        // as `m` since native 2-op-in-place forms read `Rm` before
        // overwriting `Rdn` — and `Rdn` is `ACC_REG`, not `dest`, so
        // `dest` isn't clobbered until the explicit `movHi` below (docs/
        // design.md §16 item 11).
        const rhs: Shape = operand ?? { kind: "reg", reg: dest }
        materializeShape(e, accShape, ACC_REG)
        const m = shapeToReg(e, rhs, SCRATCH_REG)
        e.emit(twoOpInPlaceNative(op, ACC_REG, m))
        if(dest !== ACC_REG) e.emit(arm.movHi(dest, ACC_REG))
        return
    }

    if(kind === "shiftImm")
    {
        if(operand === undefined || operand.kind !== "imm")
            throw new Error(`binops: ${op} shiftImm classification requires an immediate operand`)
        e.emit(shiftOpImm(op as "SHL" | "SHR" | "ASR", dest, shapeToReg(e, accShape, SCRATCH_REG), operand.value))
        return
    }

    emitAddSubRsub(e, op as "ADD" | "SUB" | "RSUB", dest, accShape, operand)
}
