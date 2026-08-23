/**
 * @ppl/jit-armv6m-prototype/test — `PEEK_PEEK` for 2-op-in-place binary ops
 * (docs/design.md §16 item 11)
 *
 * `binops.ts`'s `emitBinaryOp` used to throw for `PEEK_PEEK` combined with
 * AND/OR/XOR/MUL or a register-count shift ("2-op-in-place" — Thumb-1's
 * 2-operand encoding always reads the destination as an input, so a
 * pending value must be materialized first regardless of combo). Now it
 * treats `dest` itself as the right-hand operand, the same idiom
 * `emitAddSubRsub` already established for `PEEK_PEEK` ADD/SUB/RSUB.
 * Covers both a commutative op (AND) and a non-symmetric one (SHL, where
 * operand *order* — "acc shifted by [tos-1]'s amount", not the reverse —
 * genuinely matters).
 *
 * Hand-built via @ppl/machine's own rtl.ts constructors (same discipline
 * as rotation.test.ts) — needs an exact `PEEK_PEEK` combo, which the `ir`
 * DSL's own lowering choices don't expose directly.
 */

import { describe, test } from "node:test"
import assert from "node:assert/strict"

import { CONST, PUSH, POP, opStack, bare, validateProgram, run } from "@ppl/machine"
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

describe("PEEK_PEEK 2-op-in-place (§16 item 11)", () =>
{
    test("AND (commutative): [tos-1] = acc AND [tos-1]", () =>
    {
        const program: RtlProgram = {
            procedures: [{
                argCount: 0,
                body: [CONST(12), PUSH(), CONST(10), opStack("AND", "PEEK_PEEK"), POP(), bare("RETURN")],
            }],
        }
        const code = checkedTranslate(program)
        assert.equal(runOnQemu(code, 0), 12 & 10) // 8
    })

    test("SHL (non-symmetric): [tos-1] = acc SHL [tos-1] — operand order matters", () =>
    {
        const program: RtlProgram = {
            procedures: [{
                argCount: 0,
                body: [CONST(1), PUSH(), CONST(4), opStack("SHL", "PEEK_PEEK"), POP(), bare("RETURN")],
            }],
        }
        const code = checkedTranslate(program)
        assert.equal(runOnQemu(code, 0), 4 << 1) // acc(4) shifted left by [tos-1](1) = 8, not 1 shifted by 4
    })
})
