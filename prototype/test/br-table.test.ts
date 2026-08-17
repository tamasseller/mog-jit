/**
 * @ppl/jit-armv6m-prototype/test — BR_TABLE N>2, the shared jump-table
 * helper
 *
 * blocks.ts's own header: N ∈ {1,2} (if/if-else) fuses against a preceding
 * comparison; N>2 (a genuine multi-way selector) dispatches through a
 * shared per-procedure helper instead. The one real correctness wrinkle,
 * caught only by tracing a genuinely out-of-range selector against
 * @ppl/machine's own reference interpreter before writing any ARM: isa-core.md's
 * `acc >= N` behavior is "fall through with *no* case body run at all, acc
 * left untouched" — not "run the last case," which an `N`-entry clamped
 * table would have silently done instead. The out-of-range case below is
 * exactly what pins that down.
 *
 * Hand-built via @ppl/machine's own rtl.ts constructors, cross-checked
 * against `run()` before ever reaching QEMU (same discipline as
 * call.test.ts/deep-args.test.ts).
 */

import { describe, test } from "node:test"
import assert from "node:assert/strict"

import { PUSH, CONST, opReg, bare, brTable, validateProgram, run } from "@ppl/machine"
import type { RtlProgram } from "@ppl/machine"
import { translateProgram } from "../src/program"
import { runOnQemu } from "./qemu-run"

function checkedTranslate(program: RtlProgram): Uint16Array
{
    validateProgram(program)
    const result = run(program)
    assert.equal(result.ok, true, "reference interpreter run failed")
    const { code } = translateProgram(program)
    return code
}

describe("BR_TABLE N>2 (jump-table helper)", () =>
{
    // argCount=0, selector baked in via CONST — @ppl/machine's own `run()`
    // has no way to inject a runtime argument into the entry procedure
    // (call.test.ts's own established pattern), so a fresh program per
    // selector value is what lets every case still cross-check against the
    // reference interpreter before reaching QEMU.
    function fourCaseProgram(selector: number): RtlProgram
    {
        return {
            procedures: [{
                argCount: 0,
                body: [
                    CONST(selector),
                    brTable(4),
                    CONST(100), bare("BLOCK_END"),
                    CONST(200), bare("BLOCK_END"),
                    CONST(300), bare("BLOCK_END"),
                    CONST(400), bare("BLOCK_END"),
                    bare("RETURN"),
                ],
            }],
        }
    }

    for(const [selector, want] of [[0, 100], [1, 200], [2, 300], [3, 400]] as const)
    {
        test(`selector ${selector} dispatches to case ${selector}`, () =>
        {
            const code = checkedTranslate(fourCaseProgram(selector))
            assert.equal(runOnQemu(code, 0), want)
        })
    }

    test("out-of-range selector falls through with acc untouched, not the last case", () =>
    {
        // isa-core.md: acc >= N runs *no* case body at all — the naive
        // "clamp to N-1" scheme would instead re-run case 3 (400) here.
        assert.equal(runOnQemu(checkedTranslate(fourCaseProgram(4)), 0), 4)
        assert.equal(runOnQemu(checkedTranslate(fourCaseProgram(9)), 0), 9)
    })

    test("two independent BR_TABLE N>2 sites in one procedure share one helper", () =>
    {
        // argCount=0 — both selectors are constructed internally, since
        // `runOnQemu` only ever sets `r0` (one argument).
        const program: RtlProgram = {
            procedures: [{
                argCount: 0,
                body: [
                    CONST(1), // selector A
                    brTable(3),
                    CONST(10), bare("BLOCK_END"),
                    CONST(20), bare("BLOCK_END"),
                    CONST(30), bare("BLOCK_END"),
                    PUSH(), // stash the first switch's result as a new local (k=0)
                    CONST(2), // selector B
                    brTable(3),
                    CONST(1), bare("BLOCK_END"),
                    CONST(2), bare("BLOCK_END"),
                    CONST(3), bare("BLOCK_END"),
                    opReg("ADD", 0),
                    bare("RETURN"),
                ],
            }],
        }
        const code = checkedTranslate(program)
        // selector A=1 -> 20, selector B=2 -> 3; result 3+20=23
        assert.equal(runOnQemu(code, 0), 23)
    })
})
