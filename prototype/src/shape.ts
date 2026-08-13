/**
 * @ppl/jit-armv6m-prototype — `Shape`: a value's location right now
 *
 * The common currency between accstate.ts's CLEAN/PENDING bookkeeping and
 * binops.ts's pure instruction selection — a value is either a compile-time
 * constant not yet materialized (`imm`), or already sitting in some
 * physical register (`reg`). Neither side needs to know how the other
 * produced its `Shape`, or what it'll do with the one it hands back.
 */

import { Emitter } from "./emit"
import * as arm from "./armv6"

export type Shape =
    | { kind: "imm"; value: number }
    | { kind: "reg"; reg: number }

/** Turn any `Shape` into a concrete value sitting in `dstReg` — the one
 *  primitive every "materialize this" operation (accstate.ts's `flush`,
 *  binops.ts's own immediate-operand handling) reduces to. A no-op when
 *  `shape` is already `dstReg` itself. */
export function materializeShape(e: Emitter, shape: Shape, dstReg: number): void
{
    if(shape.kind === "imm") arm.synthesizeImm32(dstReg, shape.value).forEach(w => e.emit(w))
    else if(shape.reg !== dstReg) e.emit(arm.movHi(dstReg, shape.reg))
}

/** A `Shape` as a register, materializing into `scratchReg` only if it
 *  isn't one already — for call sites that need "some register holding
 *  this value" and don't care which, as opposed to `materializeShape`'s
 *  "specifically this one." */
export function shapeToReg(e: Emitter, shape: Shape, scratchReg: number): number
{
    if(shape.kind === "reg") return shape.reg
    materializeShape(e, shape, scratchReg)
    return scratchReg
}
