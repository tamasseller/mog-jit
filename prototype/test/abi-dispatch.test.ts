/**
 * @ppl/jit-armv6m-prototype/test — real-ABI dispatch, no eviction (Milestone 1)
 *
 * The same real dispatch table/control stack/callHelper/returnHelper
 * machinery eviction.test.ts stresses under an undersized arena, run here
 * with a generous one instead — isolates the ABI plumbing itself (the
 * prologue stub's PC-relative resume arithmetic, the control-stack push/
 * pop, the `enter_program` bootstrap record, the shared landing
 * convention) from compaction's own failure modes, before eviction.test.ts
 * adds those on top. Reuses call.test.ts's own two programs (§6's shuffle,
 * still this design's least-proven piece per docs/jit-armv6m.md §16 item 1)
 * translated through the real ABI instead of the no-eviction default, plus
 * a 3-deep call chain neither existing suite exercises.
 */

import { describe, test } from "node:test"
import assert from "node:assert/strict"

import { CONST, PUSH, LOAD, opReg, opImm, bare, call, validateProgram, run } from "@ppl/machine"
import type { RtlProgram } from "@ppl/machine"
import { translateProgramAbi } from "../src/programAbi"
import { runAbiOnQemu } from "./qemu-run-abi"

const GENEROUS_ARENA = 400 // every procedure below fits at once — no eviction should ever fire

function checkedTranslate(program: RtlProgram)
{
    validateProgram(program)
    const result = run(program)
    assert.equal(result.ok, true, "reference interpreter run failed")
    return translateProgramAbi(program)
}

describe("real ABI dispatch (no eviction)", () =>
{
    test("single-argument call, entirely acc-passed (no stack args, no shuffle)", () =>
    {
        const program: RtlProgram = {
            procedures: [
                { argCount: 0, body: [CONST(37), call(1), bare("RETURN")] },
                { argCount: 1, body: [LOAD(0), opImm("ADD", 5), bare("RETURN")] },
            ],
        }
        const procs = checkedTranslate(program)
        const result = runAbiOnQemu(procs, GENEROUS_ARENA)
        assert.equal(result.trapped, false)
        assert.equal(result.value, 42)
    })

    test("3-argument call with a phase-misaligned shuffle and surviving leftover locals", () =>
    {
        const program: RtlProgram = {
            procedures: [
                {
                    argCount: 0,
                    body: [
                        CONST(100), PUSH(),
                        CONST(200), PUSH(),
                        CONST(300), PUSH(),
                        CONST(10), PUSH(),
                        CONST(20), PUSH(),
                        CONST(999),
                        call(1),
                        opReg("ADD", 0),
                        opReg("ADD", 1),
                        opReg("ADD", 2),
                        bare("RETURN"),
                    ],
                },
                {
                    argCount: 3,
                    body: [LOAD(0), opReg("ADD", 1), opReg("ADD", 2), bare("RETURN")],
                },
            ],
        }
        const procs = checkedTranslate(program)
        const result = runAbiOnQemu(procs, GENEROUS_ARENA)
        assert.equal(result.trapped, false)
        // callee: 10 + 20 + 999 = 1029; caller: 1029 + 100 + 200 + 300 = 1629
        assert.equal(result.value, 1629)
    })

    test("3-deep call chain (A calls B calls C, back through two live control-stack frames)", () =>
    {
        const program: RtlProgram = {
            procedures: [
                { argCount: 0, body: [CONST(5), call(1), bare("RETURN")] },
                { argCount: 1, body: [LOAD(0), call(2), opImm("ADD", 1), bare("RETURN")] },
                { argCount: 1, body: [LOAD(0), opImm("ADD", 100), bare("RETURN")] },
            ],
        }
        const procs = checkedTranslate(program)
        const result = runAbiOnQemu(procs, GENEROUS_ARENA)
        assert.equal(result.trapped, false)
        // C: 5 + 100 = 105; B: 105 + 1 = 106; A returns B's result unchanged
        assert.equal(result.value, 106)
    })
})
