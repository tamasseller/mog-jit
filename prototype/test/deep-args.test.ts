/**
 * @ppl/jit-armv6m-prototype/test — locals/arguments beyond the 4-register
 * window
 *
 * Two gaps this file closes, found while working out what supporting
 * `stackArgs > WINDOW_SIZE` actually requires:
 *
 * 1. General out-of-window `LOAD`/`STORE` (and register-mode arithmetic
 *    operands/write-back) — translateProc.ts previously assumed every
 *    slot reference was in-window, which happened to hold for the whole
 *    existing corpus purely by scope, not by design. Not `CALL`-specific:
 *    any procedure with more than `WINDOW_SIZE` concurrently-live locals
 *    hits it, `CALL` or not.
 * 2. `CALL`'s own shuffle for `stackArgs ≥ WINDOW_SIZE` — `fillCalleeArgs`
 *    had a latent off-by-one (`stackArgs === WINDOW_SIZE` exactly already
 *    silently dropped arg 0, unexercised by any prior test), and once
 *    fixed, a *second* bug surfaced only by actually building this case:
 *    `reloadAfterCall` assumed everything below the caller's own resident
 *    window was unrelated leftover data, which stops holding the moment
 *    `stackArgs` exceeds that window — some of what's "deeper" was itself
 *    consumed as an argument, not surviving caller state.
 *
 * Hand-built via @ppl/machine's own rtl.ts constructors (like
 * call.test.ts/rotation.test.ts) — needs exact control over exactly which
 * slot is in/out of window at which point.
 */

import { describe, test } from "node:test"
import assert from "node:assert/strict"

import { CONST, PUSH, LOAD, STORE, opReg, opRegWriteback, bare, call, validateProgram, run } from "@ppl/machine"
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

describe("locals/arguments beyond the 4-register window", () =>
{
    test("LOAD/STORE/REG_ACC/REG_REG on slots that have fallen out of window (no CALL at all)", () =>
    {
        // Six locals (k=0..5) — the window only holds the last four (k=2..5)
        // once all six exist, so k=0 and k=1 start this test genuinely
        // spilled. Exercises every out-of-window read/write shape:
        // LOAD (k=0), REG_ACC operand (k=1), STORE (k=0), REG_REG
        // write-back — both operand-read *and* destination-write (k=1).
        const program: RtlProgram = {
            procedures: [{
                argCount: 0,
                body: [
                    CONST(10), PUSH(), // k=0
                    CONST(20), PUSH(), // k=1
                    CONST(30), PUSH(), // k=2
                    CONST(40), PUSH(), // k=3
                    CONST(50), PUSH(), // k=4 -- evicts k=0's register
                    CONST(60), PUSH(), // k=5 -- evicts k=1's register; k=0,1 now spilled
                    LOAD(0),               // acc = 10 (out-of-window LOAD)
                    opReg("ADD", 1),       // acc = 10+20=30 (out-of-window REG_ACC operand)
                    STORE(0),              // k0 := 30 (out-of-window STORE)
                    CONST(5),
                    opRegWriteback("ADD", 1), // k1 := 5+20=25 (out-of-window REG_REG: operand read *and* destination write)
                    LOAD(0),               // acc = k0 = 30
                    opReg("ADD", 1),       // acc = 30 + k1(25) = 55
                    bare("RETURN"),
                ],
            }],
        }
        const code = checkedTranslate(program)
        assert.equal(runOnQemu(code, 0), 55)
    })

    test("CALL with more stack-passed args than the window can hold (stackArgs > WINDOW_SIZE)", () =>
    {
        // Callee argCount=7 -> stackArgs=6, exceeding WINDOW_SIZE(4): the
        // callee's own in-window range (k >= argCount-4 = 3) covers only
        // arg3, arg4, arg5 (stack) + arg6 (acc) — arg0, arg1, arg2 start
        // genuinely spilled from the callee's very first instruction,
        // needing exactly the out-of-window LOAD path above.
        const program: RtlProgram = {
            procedures: [
                {
                    argCount: 0,
                    body: [
                        CONST(10), PUSH(), // arg0
                        CONST(20), PUSH(), // arg1
                        CONST(30), PUSH(), // arg2
                        CONST(40), PUSH(), // arg3
                        CONST(50), PUSH(), // arg4
                        CONST(60), PUSH(), // arg5
                        CONST(70),          // arg6 — last arg, via acc, never pushed
                        call(1),
                        bare("RETURN"),
                    ],
                },
                {
                    argCount: 7,
                    body: [
                        LOAD(0),
                        opReg("ADD", 1), opReg("ADD", 2), opReg("ADD", 3),
                        opReg("ADD", 4), opReg("ADD", 5), opReg("ADD", 6),
                        bare("RETURN"),
                    ],
                },
            ],
        }
        const code = checkedTranslate(program)
        // 10+20+30+40+50+60+70 = 280
        assert.equal(runOnQemu(code, 0), 280)
    })
})
