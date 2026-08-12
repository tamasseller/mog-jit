/**
 * @ppl/jit-armv6m-prototype — whole-program driver
 *
 * The "translate the whole program up front instead of on demand" swap
 * that makes this a prototype rather than the real JIT (this session's
 * brief): one call to `translateProc` per procedure, eagerly, instead of
 * lazily on first `CALL` through a dispatch-table trampoline (§9). Since
 * `CALL` itself isn't implemented yet (translateProc.ts's own scope note),
 * this only ever has one procedure in practice — kept as a loop anyway so
 * the shape is already right for whenever `CALL` support lands.
 */

import { validateProgram } from "@ppl/machine"
import type { RtlProgram } from "@ppl/machine"
import { translateProc } from "./translateProc"

export interface TranslatedProgram
{
    /** One native code blob per procedure, in procedure-table order. */
    procedures: readonly Uint16Array[]
}

export function translateProgram(program: RtlProgram): TranslatedProgram
{
    const stats = validateProgram(program) // isa-core.md §8 — fail fast on a malformed program before translating any of it
    const procedures = program.procedures.map((proc, i) => translateProc(proc, stats.procedures[i]!.localPeak))
    return { procedures }
}
