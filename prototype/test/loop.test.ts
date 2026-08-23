/**
 * @ppl/jit-armv6m-prototype/test — LOOP body closed by a bare terminator
 * (isa-core.md §7.2)
 *
 * §7.2: "A `LOOP`'s body block may also be closed by a terminator instead
 * of `BLOCK_END`: a loop that tests its condition once, then either runs
 * its body once and exits via `RETURN`/`TRAP` or falls through, never
 * taking the back-edge." A legitimate, validator-accepted shape, handled
 * by blocks.ts's `closeLoopBodyViaTerminator` (translateProc.ts's own
 * RETURN/TRAP cases dispatch to it whenever a frame is still open) —
 * patching the condition's own exit branch (`exitFixup`) to land right
 * here, since `closeBlockEnd`'s own back-edge path never runs when a bare
 * terminator, not `BLOCK_END`, is what actually closes the body.
 *
 * The exit tail below deliberately returns a value (999) different from
 * every `value` tried — an unpatched `exitFixup` doesn't fail loudly, it
 * decodes as "skip exactly one halfword forward," landing inside the
 * body's own code (in this shape, right on the body's own `RETURN`, still
 * holding whatever the pre-loop `CONST` left in acc) and returning
 * *that*. A same-valued tail would have made this pass by coincidence
 * even with `exitFixup` never patched at all — confirmed by hand via a
 * QEMU disassembly before this file's tail value was changed from 0 to
 * 999 to actually catch it.
 *
 * Hand-built via @ppl/machine's own rtl.ts constructors, cross-checked
 * against `run()` before ever reaching QEMU (same discipline as
 * br-table.test.ts/call.test.ts) — argCount=0, the input value baked in via
 * `CONST` (br-table.test.ts's own established pattern: `run()` has no way
 * to inject a runtime argument into the entry procedure).
 */

import { describe, test } from "node:test"
import assert from "node:assert/strict"

import { CONST, bare, validateProgram, run } from "@ppl/machine"
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

describe("LOOP", () =>
{
    describe("body closed by a bare terminator instead of BLOCK_END (isa-core.md §7.2)", () =>
    {
        // Nonzero: the condition block's exit branch is *not* taken, the
        // body runs once and returns 42 via its own bare terminator — the
        // loop's second closer, no back-edge, no second BLOCK_END. Zero:
        // the exit branch fires, skipping the body entirely, landing on the
        // tail's own `CONST 999; RETURN` right after the whole construct.
        function program(value: number): RtlProgram
        {
            return {
                procedures: [{
                    argCount: 0,
                    body: [
                        CONST(value),
                        bare("LOOP"),
                        bare("BLOCK_END"),          // closes the condition block
                        CONST(42), bare("RETURN"),  // body — bare terminator closes it
                        CONST(999), bare("RETURN"), // reached only via the cond-false exit
                    ],
                }],
            }
        }

        for(const [value, want] of [[0, 999], [1, 42], [7, 42]] as const)
        {
            test(`value ${value} -> ${want}`, () =>
            {
                const code = checkedTranslate(program(value))
                assert.equal(runOnQemu(code, 0), want)
            })
        }
    })
})
