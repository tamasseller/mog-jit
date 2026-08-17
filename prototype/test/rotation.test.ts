/**
 * @ppl/jit-armv6m-prototype/test — §10.1's rotation-eviction hazard
 *
 * docs/jit-armv6m.md §16 item 6 flagged this as "reasoned, not implemented
 * or tested" — window.ts's `pushValue` used to *throw* rather than handle
 * it, and nothing in the existing corpus exercised that path at all. Once
 * actually built, the hazard turns out not to need a rescue instruction:
 * `accState` can only depend on `physReg(evictedByPush)` — the exact
 * register a `PUSH` is about to evict — by directly referencing that same
 * slot, which makes the value about to be pushed and the value about to be
 * evicted provably identical (window.ts's `pushValue` doc comment has the
 * full argument). Both fold shapes that can create such a dependency
 * (`PENDING(Reg(...))` via `LOAD`, `CLEAN(reg)` via a destination-fold) are
 * exercised below.
 *
 * Hand-built via @ppl/machine's own rtl.ts constructors (like
 * call.test.ts), not the `ir` DSL — this needs exact control over exactly
 * which instruction is a `LOAD`/`STORE`/`PUSH` at exactly the window
 * boundary.
 */

import { describe, test } from "node:test"
import assert from "node:assert/strict"

import { CONST, PUSH, LOAD, STORE, bare, validateProgram, run } from "@ppl/machine"
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

describe("rotation-eviction fallback (§10.1)", () =>
{
    test("re-pushing the oldest in-window slot right as it would be evicted (operand-fold)", () =>
    {
        // a,b,c,d fill the 4-register window exactly (tos: 0->4). LOAD 0
        // then leaves accState depending on physReg(0) — the *exact*
        // register the very next PUSH is about to evict (evictedByPush =
        // tos - WINDOW_SIZE = 0 at tos=4).
        const program: RtlProgram = {
            procedures: [
                {
                    argCount: 0,
                    body: [
                        CONST(10), PUSH(), // a — k=0
                        CONST(20), PUSH(), // b — k=1
                        CONST(30), PUSH(), // c — k=2
                        CONST(40), PUSH(), // d — k=3
                        LOAD(0),           // acc = a; accState depends on physReg(0)
                        PUSH(),            // e = a — k=4, evicts k=0's register
                        bare("RETURN"),    // returns whatever's pending/clean — e's value
                    ],
                },
            ],
        }
        const code = checkedTranslate(program)
        assert.equal(runOnQemu(code, 0), 10)
    })

    test("re-pushing a just-overwritten oldest slot right as it would be evicted (destination-fold)", () =>
    {
        // Same window shape, but this time the dependency on physReg(0) is
        // created by STORE-folding a *new* value into slot 0 — overwriting
        // a with 99 — right before the evicting PUSH, instead of LOAD-ing
        // an unchanged one.
        const program: RtlProgram = {
            procedures: [
                {
                    argCount: 0,
                    body: [
                        CONST(10), PUSH(), // a — k=0
                        CONST(20), PUSH(), // b — k=1
                        CONST(30), PUSH(), // c — k=2
                        CONST(40), PUSH(), // d — k=3
                        CONST(99), STORE(0), // a := 99; accState depends on physReg(0)
                        PUSH(),               // e = a (now 99) — k=4, evicts k=0's register
                        bare("RETURN"),
                    ],
                },
            ],
        }
        const code = checkedTranslate(program)
        assert.equal(runOnQemu(code, 0), 99)
    })
})
