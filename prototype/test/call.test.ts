/**
 * @ppl/jit-armv6m-prototype/test — CALL, §6's own shuffle
 *
 * §6's shuffle was this design's own least-proven piece before this file
 * (and abi-dispatch.test.ts/eviction.test.ts after it) verified it end to
 * end on real QEMU — a worked example that actually calls another
 * procedure and pushes past the 4-register window, since leb128_len/the
 * core-testsuite algorithms (this package's only corpus before this file)
 * never do. Hand-built via @ppl/machine's own rtl.ts
 * constructors rather than the `ir` DSL — this needs *exact* control over
 * how many values are resident and at what phase when `CALL` fires, which
 * the DSL's own lowering choices would obscure.
 *
 * Every program here is cross-checked against `@ppl/machine`'s own `run`
 * (the reference interpreter) before ever reaching QEMU, so a real ARM
 * failure can't be misread as "my hand-written bytecode was wrong."
 */

import { describe, test } from "node:test"
import assert from "node:assert/strict"

import { CONST, PUSH, LOAD, opReg, opImm, bare, call, validateProgram, run } from "@ppl/machine"
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

describe("CALL (§6's shuffle)", () =>
{
    test("single-argument call, entirely acc-passed (no stack args, no shuffle)", () =>
    {
        // proc 0 (entry): CONST 37; CALL 1; RETURN
        // proc 1 (argCount 1): LOAD 0; ADD #5; RETURN — arg0 + 5
        const program: RtlProgram = {
            procedures: [
                { argCount: 0, body: [CONST(37), call(1), bare("RETURN")] },
                { argCount: 1, body: [LOAD(0), opImm("ADD", 5), bare("RETURN")] },
            ],
        }
        const code = checkedTranslate(program)
        assert.equal(runOnQemu(code, 0), 42)
    })

    test("3-argument call with a phase-misaligned shuffle and surviving leftover locals", () =>
    {
        // proc 0 (entry, argCount 0):
        //   push three "leftover" locals (100, 200, 300) — these must
        //   survive the call untouched, physically sharing the same
        //   4-register window as the call's own stack args once pushed;
        //   push two stack args (10, 20) for the callee, landing at
        //   window phase 3 (tos=5 at the call site) — deliberately not
        //   phase 0, so the callee's own canonical phase-0 window can
        //   only be correct if the shuffle actually re-homes them;
        //   the third (last) argument, 999, stays in acc, per §4.6.
        //   After the call returns: result + leftover0 + leftover1 + leftover2.
        // proc 1 (argCount 3): arg0 + arg1 + arg2
        const program: RtlProgram = {
            procedures: [
                {
                    argCount: 0,
                    body: [
                        CONST(100), PUSH(), // leftover local 0 — k=0
                        CONST(200), PUSH(), // leftover local 1 — k=1
                        CONST(300), PUSH(), // leftover local 2 — k=2
                        CONST(10), PUSH(),  // stack arg 0 for the callee — k=3
                        CONST(20), PUSH(),  // stack arg 1 for the callee — k=4
                        CONST(999),         // last (acc) arg for the callee — never pushed
                        call(1),
                        opReg("ADD", 0), // += leftover local 0
                        opReg("ADD", 1), // += leftover local 1
                        opReg("ADD", 2), // += leftover local 2
                        bare("RETURN"),
                    ],
                },
                {
                    argCount: 3,
                    body: [LOAD(0), opReg("ADD", 1), opReg("ADD", 2), bare("RETURN")],
                },
            ],
        }
        const code = checkedTranslate(program)
        // callee: 10 + 20 + 999 = 1029; caller: 1029 + 100 + 200 + 300 = 1629
        assert.equal(runOnQemu(code, 0), 1629)
    })
})
