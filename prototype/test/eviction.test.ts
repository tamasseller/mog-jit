/**
 * @ppl/jit-armv6m-prototype/test — eviction + compaction (Milestone 2)
 *
 * abi-dispatch.test.ts validates the real dispatch ABI alone, with a
 * generous arena so eviction never fires. This file shrinks the arena
 * instead — the same binary-generation pipeline, forced to actually run
 * runtime_host.c's eviction (global LRU minimum, nothing pinned — not
 * even the currently-suspended caller) and compaction (memmove + fix up
 * just the moved slot's own code_ptr, §11's position-independence), then
 * recompile-and-résumé through a saved `(proc_idx, offset)` — §7's
 * canonical-phase invariant: recompiling the same procedure from the same
 * flash blob must reproduce the same layout, or the saved offset would no
 * longer point at the right place.
 *
 * `arenaSize` is derived from the program's own compiled sizes rather than
 * a hand-picked constant, so these tests keep forcing eviction even if the
 * exact byte counts drift — total minus the smallest procedure's own size
 * guarantees not everything fits at once, while still being at least as
 * large as any single procedure, so forward progress is always possible
 * (never a thrash that can't converge).
 */

import { describe, test } from "node:test"
import assert from "node:assert/strict"

import { CONST, LOAD, opImm, bare, call, validateProgram, run } from "@ppl/machine"
import type { RtlProgram } from "@ppl/machine"
import { translateProgramAbi } from "../src/programAbi"
import { runAbiOnQemu } from "./qemu-run-abi"

function checkedTranslate(program: RtlProgram)
{
    validateProgram(program)
    const result = run(program)
    assert.equal(result.ok, true, "reference interpreter run failed")
    return translateProgramAbi(program)
}

function sizesOf(procs: readonly { code: Uint16Array }[]): number[]
{
    return procs.map(p => p.code.length * 2)
}

/** Smallest arena that can't hold every procedure at once, but can always
 *  hold any single one of them — forces eviction without risking a thrash
 *  that never converges. */
function forcedEvictionArena(sizes: readonly number[]): number
{
    const total = sizes.reduce((a, b) => a + b, 0)
    return total - Math.min(...sizes) + 4
}

describe("eviction + compaction", () =>
{
    test("3-deep call chain, arena too small for all three procedures at once", () =>
    {
        // Same chain as abi-dispatch.test.ts's own — here the point is that
        // it *cannot* all be resident together, so compiling the deepest
        // call forces evicting an ancestor (possibly the entry procedure
        // itself, still suspended on the control stack), which then has to
        // be recompiled from scratch when its own RETURN eventually fires.
        const program: RtlProgram = {
            procedures: [
                { argCount: 0, body: [CONST(5), call(1), bare("RETURN")] },
                { argCount: 1, body: [LOAD(0), call(2), opImm("ADD", 1), bare("RETURN")] },
                { argCount: 1, body: [LOAD(0), opImm("ADD", 100), bare("RETURN")] },
            ],
        }
        const procs = checkedTranslate(program)
        const arenaSize = forcedEvictionArena(sizesOf(procs))
        const result = runAbiOnQemu(procs, arenaSize)
        assert.equal(result.trapped, false)
        assert.equal(result.value, 106)
    })

    test("caller and callee never coresident: eviction and recompile in both directions", () =>
    {
        // A calls B; the arena fits only one of the two at a time, so
        // compiling B evicts A (still suspended on the control stack, mid-
        // call) — then B's own RETURN has to recompile A from scratch
        // before it can resume. Cross-recompilation in both directions
        // within a single call/return round trip, not just one.
        const program: RtlProgram = {
            procedures: [
                { argCount: 0, body: [CONST(1), call(1), opImm("ADD", 1000), bare("RETURN")] },
                { argCount: 1, body: [LOAD(0), opImm("ADD", 1), bare("RETURN")] },
            ],
        }
        const procs = checkedTranslate(program)
        const sizes = sizesOf(procs)
        // Fits at most one of the two at a time.
        const arenaSize = Math.max(...sizes) + 4
        const result = runAbiOnQemu(procs, arenaSize)
        assert.equal(result.trapped, false)
        // B: 1 + 1 = 2; A: 2 + 1000 = 1002
        assert.equal(result.value, 1002)
    })

    test("RESOURCE_ERROR: a single procedure larger than the whole arena", () =>
    {
        const bigBody = [CONST(0), ...Array.from({ length: 40 }, () => opImm("ADD", 1)), bare("RETURN")]
        const program: RtlProgram = { procedures: [{ argCount: 0, body: bigBody }] }
        const procs = checkedTranslate(program)
        const size = sizesOf(procs)[0]!
        const result = runAbiOnQemu(procs, Math.min(20, size - 4))
        assert.equal(result.trapped, true)
        assert.equal(result.value, 0x52455343) // "RESC" — runtime_host.c's RESOURCE_ERROR_CODE
    })
})
