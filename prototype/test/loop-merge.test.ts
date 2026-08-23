/**
 * @ppl/jit-armv6m-prototype/test — LOOP's back-edge control-flow merge
 * (docs/design.md §16 item 3)
 *
 * blocks.ts's `openLoop`/`closeBlockEnd` compile the condition sub-block's
 * code exactly once, but two different runtime paths reach it: the initial
 * fall-through (from whatever preceded LOOP) and the body's own back-edge.
 * Neither path flushed `accState` before this fix — so a condition that
 * folds a *pending* value (rather than starting with a fresh producer)
 * compiles against whichever `accState` preceded LOOP the first time, and
 * the back-edge silently reuses that same, now-stale compiled instruction
 * regardless of what the body actually left pending.
 *
 * Hand-built via @ppl/machine's own rtl.ts constructors (same discipline as
 * loop.test.ts/rotation.test.ts) — needs exact control over accState's
 * shape entering the condition block, which the `ir` DSL's own lowering
 * choices would obscure.
 */

import { describe, test } from "node:test"
import assert from "node:assert/strict"

import { CONST, PUSH, LOAD, STORE, opReg, opImm, bare, validateProgram, run } from "@ppl/machine"
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

describe("LOOP's back-edge merge (§16 item 3)", () =>
{
    test("a condition that folds a pending immediate must re-see what the body leaves pending, not what preceded LOOP", () =>
    {
        // counter/limit: entering the loop, acc is PENDING(Imm(5)) — a
        // "limit" the condition folds directly (no fresh LOAD precedes the
        // comparison — deliberately, to expose the merge hazard). The body
        // increments a real counter, then leaves acc PENDING(Imm(0)) — a
        // *different* limit — before the back-edge.
        //
        // Correct (the condition block re-sees whatever the body actually
        // left pending, on both the initial entry and the back-edge):
        //   check 1 (limit=5): 5 > 0 -> enter; counter=1, next limit=0.
        //   check 2 (limit=0): 0 > 1 -> false -> exit. Returns counter=1.
        // Buggy (the condition's fold compiled once against the *first*
        // limit, reused verbatim on the back-edge, ignoring the body's own
        // CONST(0)): check 2 still tests 5 > 1 -> true -> keeps entering
        // until the baked "5" finally loses to the growing counter.
        // Returns 5.
        const program: RtlProgram = {
            procedures: [{
                argCount: 0,
                body: [
                    CONST(0), PUSH(),                       // slot0 = counter = 0
                    CONST(5),                                 // acc = PENDING(Imm(5)) — "limit"
                    bare("LOOP"),
                        opReg("GT_U", 0), bare("BLOCK_END"),   // condition: limit > counter?
                        LOAD(0), opImm("ADD", 1), STORE(0),     // counter += 1
                        CONST(0),                                // next limit = 0
                        bare("BLOCK_END"),                        // back-edge
                    LOAD(0), bare("RETURN"),
                ],
            }],
        }
        const code = checkedTranslate(program)
        assert.equal(runOnQemu(code, 0), 1)
    })
})
