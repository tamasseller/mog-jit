/**
 * @ppl/jit-armv6m-prototype/test — blocks.ts's `emitGuardedBranch`
 * (docs/design.md §16 item 5)
 *
 * `emitGuardedBranch` picks the short (bare conditional branch) form when
 * `maxSpanBytes` can prove the guarded span fits Thumb's ±252-byte
 * conditional-branch range, else the invert-and-long-branch idiom. Every
 * other test in this suite has small enough bodies to take the short form
 * (it's what they've always emitted) — this file is the one that forces
 * the long form at both of its call sites (`openBrTable`'s if/if-else
 * fusion, `closeBlockEnd`'s loop-exit) and checks it on real QEMU, in both
 * directions (branch taken and not taken).
 *
 * Hand-built via @ppl/machine's own rtl.ts constructors (same discipline
 * as loop.test.ts/comparison-fusion.test.ts) — needs an oversized,
 * mechanically-generated body, which the `ir` DSL isn't a natural fit for.
 */

import { describe, test } from "node:test"
import assert from "node:assert/strict"

import { CONST, PUSH, LOAD, STORE, opImm, bare, brTable, validateProgram, run } from "@ppl/machine"
import type { RtlProgram, RtlInstr } from "@ppl/machine"
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

// Comfortably past blocks.ts's own SAFE_COND_BRANCH_SPAN under its
// per-instruction bound (ORDINARY_MAX_BYTES=16 * 20 == 320 > 240) — real
// emitted size stays tiny (each `ADD #1` is one Thumb instruction), so this
// only ever exercises the *conservative* long-form choice, never a program
// that's genuinely out of the real ±252-byte range.
const OVERSIZED_CASE: readonly RtlInstr[] = [CONST(0), ...Array.from({ length: 20 }, () => opImm("ADD", 1))]

describe("emitGuardedBranch: forced long form (§16 item 5)", () =>
{
    describe("openBrTable's if/if-else fusion", () =>
    {
        function program(selector: number): RtlProgram
        {
            return {
                procedures: [{
                    argCount: 0,
                    body: [
                        CONST(selector),
                        brTable(2),
                            ...OVERSIZED_CASE, bare("BLOCK_END"), // case 0 (selector falsy): acc = 20
                            CONST(99), bare("BLOCK_END"),          // case 1 (selector truthy): acc = 99
                        bare("RETURN"),                             // merge point: return whichever case ran
                    ],
                }],
            }
        }

        for(const [selector, want] of [[0, 20], [1, 99]] as const)
        {
            test(`selector ${selector} -> ${want}`, () =>
            {
                const code = checkedTranslate(program(selector))
                assert.equal(runOnQemu(code, 0), want)
            })
        }
    })

    describe("closeBlockEnd's loop-exit branch", () =>
    {
        // slot0 = a real counter, incremented by the oversized body every
        // iteration; the loop itself runs a small, fixed number of times
        // (bounded by a *separate*, freshly-LOADed condition, not by
        // anything OVERSIZED_CASE computes) so this terminates quickly
        // regardless of which branch form actually ran.
        function program(iterations: number): RtlProgram
        {
            return {
                procedures: [{
                    argCount: 0,
                    body: [
                        CONST(0), PUSH(),              // slot0 = counter = 0
                        bare("LOOP"),
                            LOAD(0), opImm("LT_U", iterations), bare("BLOCK_END"), // condition: counter < iterations?
                            ...OVERSIZED_CASE,                                       // oversized, side-effect-free body padding
                            LOAD(0), opImm("ADD", 1), STORE(0),                       // counter += 1
                            bare("BLOCK_END"),                                         // back-edge
                        LOAD(0), bare("RETURN"),
                    ],
                }],
            }
        }

        for(const iterations of [0, 3])
        {
            test(`${iterations} iterations`, () =>
            {
                const code = checkedTranslate(program(iterations))
                assert.equal(runOnQemu(code, 0), iterations)
            })
        }
    })
})
