/**
 * @ppl/jit-armv6m-prototype/test — §10.1's immediate-side mirror-table
 * optimization (blocks.ts's `emitComparison`)
 *
 * `acc` holding a small immediate compared against a register operand
 * (`5 < r0`, say) used to always materialize the immediate into `ACC_REG`
 * first — correct, but one instruction longer than necessary, since Thumb's
 * `CMP` can take the register operand directly as `Rn` if the comparison
 * is mirrored (`r0 > 5` instead of `5 < r0`) and the condition swapped to
 * match. argCount=0, the "register operand" a baked-in `CONST`+`PUSH` (a
 * fresh program per value) — br-table.test.ts's own established pattern —
 * so `run()` genuinely cross-checks each value instead of just whatever
 * `run()` would default a real argument to.
 */

import { describe, test } from "node:test"
import assert from "node:assert/strict"

import { CONST, PUSH, opReg, bare, brTable, validateProgram, run } from "@ppl/machine"
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

describe("comparison fusion — immediate-side mirror-table optimization", () =>
{
    // CONST(value); PUSH() puts `value` at slot 0 (a real register
    // operand); CONST(5) then leaves acc PENDING(imm 5) — exactly the
    // "acc holds a small immediate, operand is a register" shape the
    // mirror path targets — fused against it via opReg's REG_ACC combo.
    // `BR_TABLE 2`'s own selector is the comparison's boolean-as-integer
    // result (false→case[0], true→case[1] — confirmed against `run()`,
    // not assumed), so case[0] is the "5 < value is false" branch.
    function program(value: number): RtlProgram
    {
        return {
            procedures: [{
                argCount: 0,
                body: [
                    CONST(value), PUSH(),
                    CONST(5), opReg("LT_S", 0), brTable(2),
                    CONST(100), bare("BLOCK_END"), // case[0]: 5 < value is false
                    CONST(200), bare("BLOCK_END"), // case[1]: 5 < value is true
                    bare("RETURN"),
                ],
            }],
        }
    }

    for(const [value, want] of [[10, 200], [3, 100], [5, 100]] as const)
    {
        test(`5 < ${value} is ${want === 200} → ${want}`, () =>
        {
            const code = checkedTranslate(program(value))
            assert.equal(runOnQemu(code, 0), want)
        })
    }

    test("signed vs. unsigned mirroring stays on its own side of the line", () =>
    {
        // GE_U mirrors to LE_U (isa-core.md §10.1) — a genuinely different
        // condition code (HS vs LS) than the signed pair (GE/LE) would be,
        // so this pins down the mirror table isn't silently dropping the
        // signedness suffix. 0x80000000 is negative signed, huge unsigned —
        // `3 >=_u value` is false only when read as unsigned.
        function geUProgram(value: number): RtlProgram
        {
            return {
                procedures: [{
                    argCount: 0,
                    body: [
                        CONST(value), PUSH(),
                        CONST(3), opReg("GE_U", 0), brTable(2),
                        CONST(11), bare("BLOCK_END"), // case[0]: 3 >=_u value is false
                        CONST(22), bare("BLOCK_END"), // case[1]: 3 >=_u value is true
                        bare("RETURN"),
                    ],
                }],
            }
        }
        assert.equal(runOnQemu(checkedTranslate(geUProgram(0x80000000)), 0), 11)
        assert.equal(runOnQemu(checkedTranslate(geUProgram(2)), 0), 22)
    })
})
