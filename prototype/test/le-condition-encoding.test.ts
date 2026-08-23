/**
 * @ppl/jit-armv6m-prototype/test — armv6.ts's own `isCondBranch` off-by-one
 * for `Condition.LE`
 *
 * `isCondBranch` checked `cond < 0b1101`, excluding `Condition.LE`
 * (`0b1101`) itself — this codebase's own *largest* valid condition
 * (`inverse()`'s own assert has the same ceiling: `assert(c <=
 * Condition.LE)`). Cond fields `0b1110`/`0b1111` are UDF/SVC, never a real
 * branch condition — those are the two values worth excluding, not LE.
 *
 * The bug is silent, not loud: `patchBranch` treats any halfword
 * `isCondBranch` rejects as *unconditional*, so an LE-conditioned
 * placeholder branch gets routed into `setBranchOffset`'s own assert
 * (`isn >> 11 === 0b11100`), which fails immediately — reachable via
 * completely ordinary bytecode:
 *   - `blocks.ts`'s `emitComparison` maps `LE_S` directly to
 *     `Condition.LE` (`DIRECT_CONDITION`) and `GE_S` to `Condition.LE`
 *     when mirrored (`MIRRORED_CONDITION`) — either one used as a bare
 *     `if`/`if-else` guard (`openBrTable`) hits this on the very first
 *     `emitGuardedBranch` call, with no loop or inversion involved.
 *   - Any `LOOP` whose exit condition is `GT_S`'s own inverse (the
 *     ordinary `while (x > 0)` idiom) hits it too, since
 *     `translateProc.ts`'s own loop-exit logic inverts the condition
 *     block's `trueCondition`, and `inverse(GT) === LE`.
 *
 * Confirmed via a throwaway repro before this fix: both shapes below threw
 * `"not an unconditional branch: 0xdd00"` out of `getBranchCondtion`'s
 * caller chain on the pre-fix `armv6.ts`, and compiled + ran correctly on
 * real QEMU hardware afterward.
 */

import { describe, test } from "node:test"
import assert from "node:assert/strict"

import { CONST, PUSH, LOAD, STORE, opImm, bare, brTable, validateProgram, run } from "@ppl/machine"
import type { RtlProgram } from "@ppl/machine"
import { translateProgram } from "../src/program"
import { runOnQemu } from "./qemu-run"

function checkedTranslate(program: RtlProgram): Uint16Array
{
    validateProgram(program)
    const result = run(program)
    assert.equal(result.ok, true, "reference interpreter run failed")
    return translateProgram(program).code
}

describe("Condition.LE branch encoding", () =>
{
    describe("if/else guarded directly by LE_S (isCondBranch's own DIRECT_CONDITION case)", () =>
    {
        function program(x: number): RtlProgram
        {
            return {
                procedures: [{
                    argCount: 0,
                    body: [
                        CONST(x), opImm("LE_S", 5), brTable(2),
                            CONST(100), bare("BLOCK_END"),
                            CONST(200), bare("BLOCK_END"),
                        bare("RETURN"),
                    ],
                }],
            }
        }

        for(const x of [3, 5, 9])
        {
            test(`x = ${x}`, () =>
            {
                const code = checkedTranslate(program(x))
                const want = run(program(x)).acc
                assert.equal(runOnQemu(code, 0), want)
            })
        }
    })

    describe("LOOP exit via GT_S's own inverse (the ordinary while(x > 0) idiom)", () =>
    {
        function program(x: number): RtlProgram
        {
            return {
                procedures: [{
                    argCount: 0,
                    body: [
                        CONST(x), PUSH(),
                        bare("LOOP"),
                            LOAD(0), opImm("GT_S", 0),
                        bare("BLOCK_END"),
                            LOAD(0), opImm("SUB", 1), STORE(0),
                        bare("BLOCK_END"),
                        LOAD(0), bare("RETURN"),
                    ],
                }],
            }
        }

        for(const x of [0, 1, 5])
        {
            test(`x = ${x} counts down to 0`, () =>
            {
                const code = checkedTranslate(program(x))
                assert.equal(runOnQemu(code, 0), 0)
            })
        }
    })
})
