/**
 * @ppl/jit-armv6m-prototype/test — enter_program's *_on_stack/_split variants
 *
 * Exercises the two layout-agnostic entry points (runtime_host.h):
 * `enter_program_on_stack` puts `Runtime`/its dispatch table/the operand
 * stack *and* the compiled-code arena all on the current C stack;
 * `enter_program_split` keeps the arena in its own separate buffer
 * (standing in for a distinct SRAM bank/CCM) while everything else still
 * lives on the C stack. Both share the same stack-usage check
 * (`requiredStackBytes`, runtime_host.cpp) before either one touches any
 * memory or calls into `enter_dispatch` at all.
 *
 * Reuses abi-dispatch.test.ts's own programs, but captures
 * `validateProgram`'s real `ProgramStats` this time (instead of
 * discarding it, as `checkedTranslate` there does) to derive
 * `operandStackBytes` for real — matching runtime_host.h's own
 * documented "how to size this" story rather than a placeholder number.
 * `maxCallDepth` is the program's own known live-call-record depth at
 * its deepest point (one less than call-chain length: the innermost,
 * currently-executing procedure never has a pushed record of its own,
 * only its callers do) — not yet something `validateProgram` computes,
 * so hand-derived here from each program's own, small, known shape.
 */

import { describe, test } from "node:test"
import assert from "node:assert/strict"

import { CONST, LOAD, opImm, call, bare, validateProgram, run } from "@ppl/machine"
import type { RtlProgram, ProgramStats } from "@ppl/machine"
import { translateProgramAbi } from "../src/programAbi"
import { runAbiOnStack, runAbiSplit } from "./qemu-run-abi"

const GENEROUS_ARENA = 400 // every procedure below fits at once — no eviction should ever fire

/* Margin *above* `__bss_end` (qemu-run-abi.ts's own PROLOGUE), not a raw
 * subtraction from entry `sp`: RAM here is a real, fixed 4096 bytes
 * (jit-armv6m/src/linker.ld), so a "generous"-looking number subtracted
 * from `sp` can walk straight past the bottom of RAM without ever
 * landing on a real address — invisible for `runAbiSplit` (`stackLimit`
 * there is only ever compared against) but not for `runAbiOnStack`,
 * which now anchors its code arena *at* `stackLimit` and writes compiled
 * code there (runtime_host.cpp's own doc comment on why). */
const GENEROUS_SLACK = 64

/** `operandStackBytes` from `validateProgram`'s tight `totalDepth` — the
 *  *whole* bound, not discounted for the 4-register window: how much the
 *  window actually absorbs at any given moment depends on call-boundary
 *  argument shuffling (src/window.ts's own "spillForCall"), not just
 *  abstract depth, so crediting a flat 4 slots isn't safe in general (a
 *  worst-case path ending in a pure acc-in/acc-out leaf credits zero).
 *  See runtime_host.cpp's own `requiredStackBytes` doc comment. */
function operandStackBytesOf(stats: ProgramStats): number
{
    return stats.totalDepth * 4
}

function checkedTranslate(program: RtlProgram)
{
    const stats = validateProgram(program)
    const result = run(program)
    assert.equal(result.ok, true, "reference interpreter run failed")
    return { procs: translateProgramAbi(program), stats }
}

describe("enter_program_on_stack / enter_program_split", () =>
{
    test("on_stack: single-argument call, generous stack", () =>
    {
        const program: RtlProgram = {
            procedures: [
                { argCount: 0, body: [CONST(37), call(1), bare("RETURN")] },
                { argCount: 1, body: [LOAD(0), opImm("ADD", 5), bare("RETURN")] },
            ],
        }
        const { procs, stats } = checkedTranslate(program)
        const result = runAbiOnStack(procs, GENEROUS_ARENA, {
            operandStackBytes: operandStackBytesOf(stats),
            maxCallDepth: 1, // A calls B — one live record while B executes
            slack: GENEROUS_SLACK,
        })
        assert.equal(result.trapped, false)
        assert.equal(result.value, 42)
    })

    test("split: 3-deep call chain, code arena in its own separate buffer", () =>
    {
        const program: RtlProgram = {
            procedures: [
                { argCount: 0, body: [CONST(5), call(1), bare("RETURN")] },
                { argCount: 1, body: [LOAD(0), call(2), opImm("ADD", 1), bare("RETURN")] },
                { argCount: 1, body: [LOAD(0), opImm("ADD", 100), bare("RETURN")] },
            ],
        }
        const { procs, stats } = checkedTranslate(program)
        const result = runAbiSplit(procs, GENEROUS_ARENA, {
            operandStackBytes: operandStackBytesOf(stats),
            maxCallDepth: 2, // A→B→C — two live records while C executes
            slack: GENEROUS_SLACK,
        })
        assert.equal(result.trapped, false)
        // C: 5 + 100 = 105; B: 105 + 1 = 106; A returns B's result unchanged
        assert.equal(result.value, 106)
    })

    test("on_stack: stack-usage check rejects before touching anything", () =>
    {
        const program: RtlProgram = {
            procedures: [
                { argCount: 0, body: [CONST(37), call(1), bare("RETURN")] },
                { argCount: 1, body: [LOAD(0), opImm("ADD", 5), bare("RETURN")] },
            ],
        }
        const { procs, stats } = checkedTranslate(program)
        const result = runAbiOnStack(procs, GENEROUS_ARENA, {
            operandStackBytes: operandStackBytesOf(stats),
            maxCallDepth: 1,
            slack: "reject", // stackLimit == entry sp — any nonzero requirement fails
        })
        assert.equal(result.trapped, true)
        assert.equal(result.value, 0x52455343) // RESOURCE_ERROR_CODE, "RESC"
    })
})
