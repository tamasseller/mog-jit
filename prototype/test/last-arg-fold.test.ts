/**
 * @ppl/jit-armv6m-prototype/test — last-argument-as-a-fold
 * (docs/design.md §16 items 13/14 merged)
 *
 * `translateProc`'s callee-side prologue used to flush the incoming last
 * argument into its own frame slot unconditionally, paying that cost even
 * when the body never reads the slot at all. Now it stays a plain
 * pending producer (exactly like a `CALL`'s own return value) whenever a
 * whole-body reference count *proves* nothing needs `phys(argCount-1)`
 * actually populated: zero references, or exactly one and it's `body[0]`'s
 * own `LOAD` of that slot.
 *
 * Uses `translateProc` directly rather than `checkedTranslate`'s usual
 * `run()` cross-check — matching leb128.test.ts's own established
 * pattern here — since `vm.ts`'s `run()` always invokes procedure 0 with
 * zero arguments, which can't stand in for a real one-argument entry
 * point on QEMU.
 */

import { describe, test } from "node:test"
import assert from "node:assert/strict"

import { LOAD, opImm, bare, validateProgram } from "@ppl/machine"
import type { RtlProc } from "@ppl/machine"
import { translateProc } from "../src/translateProc"
import { runOnQemu } from "./qemu-run"

describe("last-argument fold (§16 items 13/14)", () =>
{
    test("single reference, at body[0]'s own LOAD: correct, and strictly smaller than the eager-flush fallback", () =>
    {
        // Same net computation (arg + 5), but the second procedure reads
        // its own last argument twice — refs.length is 2, not 1, so the
        // fold can't apply and the eager flush (plus the now-unfoldable
        // first LOAD) has to run instead.
        const folded: RtlProc = { argCount: 1, body: [LOAD(0), opImm("ADD", 5), bare("RETURN")] }
        const unfolded: RtlProc = { argCount: 1, body: [LOAD(0), LOAD(0), opImm("ADD", 5), bare("RETURN")] }
        validateProgram({ procedures: [folded] })
        validateProgram({ procedures: [unfolded] })
        const { code: foldedCode } = translateProc(folded)
        const { code: unfoldedCode } = translateProc(unfolded)
        assert.ok(foldedCode.length < unfoldedCode.length,
            `expected the folded procedure (${foldedCode.length} bytes) to be smaller than the unfolded one (${unfoldedCode.length} bytes)`)

        for(const v of [0, 10, 100])
        {
            assert.equal(runOnQemu(foldedCode, v), v + 5)
            assert.equal(runOnQemu(unfoldedCode, v), v + 5)
        }
    })

    test("zero references: the argument is never read via LOAD, only through acc's own propagation to RETURN", () =>
    {
        const proc: RtlProc = { argCount: 1, body: [bare("RETURN")] }
        validateProgram({ procedures: [proc] })
        const { code } = translateProc(proc)

        // Values with bit 31 clear only — `runOnQemu`'s own return
        // convention (qemu-run.ts) reads that bit as "this procedure
        // TRAPped," so a genuinely unmodified negative-looking argument
        // would be misread as a trap, not a translator bug.
        for(const v of [0, 1, 42, 0x7fffffff]) assert.equal(runOnQemu(code, v), v >>> 0)
    })
})
