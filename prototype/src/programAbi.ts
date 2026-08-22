/**
 * @ppl/jit-armv6m-prototype — whole-program driver, real ABI variant
 *
 * program.ts's counterpart for the ABI-real call/return strategy
 * (translateProc.ts's `abiRealStrategy`) — but simpler, not a fork with
 * more moving parts: every procedure is translated independently and
 * handed back as its own self-contained `[stub][body]` blob, with no
 * concatenation and no cross-procedure `BL`-patching pass at all, since
 * nothing in this ABI ever bakes in another procedure's absolute address
 * (§9/§11 — `CALL`/`RETURN` only ever reference `Q_idx`/`proc_idx`, both
 * resolved at runtime through the dispatch table). Each blob is exactly
 * what a real translator would produce to copy into the arena — and
 * exactly what test/qemu-run-abi.ts's mock translator *does* copy in,
 * unmodified, standing in for real compilation (see its own header).
 */

import { validateProgram } from "@ppl/machine"
import type { RtlProgram } from "@ppl/machine"
import { translateProc, abiRealStrategy } from "./translateProc"

export interface AbiCompiledProc
{
    /** This procedure's own `[stub][body]` blob — position-independent,
     *  safe to copy to any runtime address (§11). */
    readonly code: Uint16Array
}

export function translateProgramAbi(program: RtlProgram): readonly AbiCompiledProc[]
{
    validateProgram(program) // isa-core.md §8 — fail fast, same discipline as program.ts
    const calleeArgCounts = program.procedures.map(p => p.argCount)
    return program.procedures.map((proc, procIdx) =>
    {
        const { code, callSites } = translateProc(proc, calleeArgCounts, abiRealStrategy(procIdx, proc))
        if(callSites.length > 0)
            throw new Error(`translateProgramAbi: proc ${procIdx} left unlinked call sites — abiRealStrategy should never produce any`)
        return { code }
    })
}
