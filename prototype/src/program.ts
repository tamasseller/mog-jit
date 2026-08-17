/**
 * @ppl/jit-armv6m-prototype — whole-program driver
 *
 * The "translate the whole program up front instead of on demand" swap
 * that makes this a prototype rather than the real JIT (this session's
 * brief): one call to `translateProc` per procedure, eagerly, instead of
 * lazily on first `CALL` through a dispatch-table trampoline (§9) — and,
 * following from that, linking is a single flat pass here too, rather than
 * §9's own lazy-compile-and-patch-the-table-slot scheme. Each procedure is
 * translated independently (translateProc.ts is deliberately context-free,
 * per §4/§6) and returns its own code plus any pending `CALL` sites; this
 * module concatenates every procedure's code into one buffer, in
 * procedure-table order, and only then can resolve each `CALL`'s `BL` —
 * the target's own final offset isn't known until every earlier (and
 * later) procedure's length is.
 */

import { validateProgram } from "@ppl/machine"
import type { RtlProgram } from "@ppl/machine"
import { translateProc } from "./translateProc"
import * as arm from "./armv6"

export interface TranslatedProgram
{
    /** Every procedure's code, concatenated in procedure-table order —
     *  cross-procedure `CALL`s are plain, already-resolved `BL`s. */
    code: Uint16Array
    /** Byte offset of each procedure's own entry point within `code`, in
     *  procedure-table order — offset 0 (procedure 0's own start) is the
     *  whole program's entry point. */
    procOffsets: readonly number[]
}

export function translateProgram(program: RtlProgram): TranslatedProgram
{
    validateProgram(program) // isa-core.md §8 — fail fast on a malformed program before translating any of it
    const calleeArgCounts = program.procedures.map(p => p.argCount)
    const translated = program.procedures.map(proc => translateProc(proc, calleeArgCounts))

    const procOffsets: number[] = []
    let cursor = 0
    for(const t of translated) { procOffsets.push(cursor); cursor += t.code.length * 2 }

    const code = new Uint16Array(cursor / 2)
    for(let i = 0; i < translated.length; i++)
        code.set(translated[i]!.code, procOffsets[i]! / 2)

    for(let i = 0; i < translated.length; i++)
    {
        for(const site of translated[i]!.callSites)
        {
            const siteOffset = procOffsets[i]! + site.siteOffset
            const targetOffset = procOffsets[site.calleeIndex]!
            const [hw1, hw2] = arm.bl(targetOffset - (siteOffset + 4))
            code[siteOffset / 2] = hw1
            code[siteOffset / 2 + 1] = hw2
        }
    }

    return { code, procOffsets }
}
