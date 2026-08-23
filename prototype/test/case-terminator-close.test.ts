/**
 * @ppl/jit-armv6m-prototype/test — a `BR_TABLE` case closed by a bare
 * terminator instead of `BLOCK_END` (isa-core.md §4.5/§7.1)
 *
 * Same allowance loop.test.ts already exercises for a LOOP body, but for
 * a `case`: `validate.ts`'s own structural walk accepts a case closing via
 * RETURN/TRAP (its own `walk` returns on either `BLOCK_END` *or* a
 * terminator, treating them identically for "where does the next sibling
 * start"), so a lowerer producing this shape isn't malformed input — it's
 * translateProc.ts/blocks.ts's own `Frame` bookkeeping that has to keep up.
 * Before `blocks.ts`'s `closeCaseViaTerminator` existed, a non-last case
 * closing this way left `nextCaseFixup` (the dispatch guard's own "skip to
 * the next case" branch) permanently unpatched — a real miscompilation,
 * not just a missed accounting step — and a *last* case closing this way
 * left `remaining` never decremented at all, which surfaced as
 * translateProc.ts's own "procedure body ended with an open block" error
 * (confirmed by hand before the fix: constructing case 2 below with a bare
 * `RETURN` throws exactly that).
 */

import { describe, test } from "node:test"
import assert from "node:assert/strict"

import { CONST, PUSH, LOAD, opImm, bare, brTable, validateProgram, run } from "@ppl/machine"
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

describe("BR_TABLE case closed by a bare terminator (isa-core.md §4.5/§7.1)", () =>
{
    describe("non-last case closes via RETURN, next case still gets its dispatch guard patched", () =>
    {
        // case 0 (n <= 10) returns directly — no BLOCK_END, no fall-through
        // into case 1's own code. case 1 (n > 10) closes normally. If
        // `nextCaseFixup` were left unpatched, its placeholder (all-zero)
        // displacement would happen to skip exactly one halfword forward —
        // landing inside case 0's own body instead of case 1's, for any n
        // that should have dispatched to case 1.
        function program(n: number): RtlProgram
        {
            return {
                procedures: [{
                    argCount: 0,
                    body: [
                        CONST(n), opImm("GT_U", 10), brTable(2),
                            CONST(111), bare("RETURN"),          // case 0 (n <= 10) — bare terminator
                            CONST(222), bare("BLOCK_END"),        // case 1 (n > 10) — normal close
                        bare("RETURN"),
                    ],
                }],
            }
        }

        for(const [n, want] of [[0, 111], [10, 111], [11, 222], [50, 222]] as const)
        {
            test(`n=${n} -> ${want}`, () =>
            {
                assert.equal(runOnQemu(checkedTranslate(program(n)), 0), want)
            })
        }
    })

    describe("every case closes via its own bare terminator, including the last", () =>
    {
        // Both cases return directly — no BLOCK_END anywhere in the
        // construct itself. Before the fix this threw at translation time
        // ("procedure body ended with an open block"): the last case's own
        // close never ran, so `remaining` stayed at its pre-decrement value
        // and translateBody's own while loop ran off the end of the byte
        // stream still believing a case was open.
        //
        // The trailing `CONST(0); RETURN` is never actually reached for
        // any `n` tried below (the fused comparison guarantees acc ∈
        // {0,1}, always a valid case index) — but validate.ts's own
        // BR_TABLE model doesn't reason about specific comparison results,
        // so it still conservatively requires something reachable right
        // after the construct for isa-core.md's own "acc >= N: fall
        // through, no case body runs" default path. Unrelated to the bug
        // this test targets; just what makes the program well-formed.
        function program(n: number): RtlProgram
        {
            return {
                procedures: [{
                    argCount: 0,
                    body: [
                        CONST(n), opImm("GT_U", 10), brTable(2),
                            CONST(111), bare("RETURN"), // case 0
                            CONST(222), bare("RETURN"), // case 1 — the construct's own last case
                        CONST(0), bare("RETURN"),
                    ],
                }],
            }
        }

        for(const [n, want] of [[0, 111], [11, 222]] as const)
        {
            test(`n=${n} -> ${want}`, () =>
            {
                assert.equal(runOnQemu(checkedTranslate(program(n)), 0), want)
            })
        }
    })

    describe("BR_TABLE N>2 (jump-table helper): an early case closes via RETURN", () =>
    {
        // Same gap, `table`-based dispatch instead of a fused comparison
        // guard (blocks.ts's `openBrTableJump`) — `resolveCaseClose`'s own
        // `table.nextFixupSlot`/`table.endSlot` branch, not `nextCaseFixup`.
        function program(selector: number): RtlProgram
        {
            return {
                procedures: [{
                    argCount: 0,
                    body: [
                        CONST(selector),
                        brTable(4),
                        CONST(100), bare("RETURN"),      // case 0 — bare terminator (non-last)
                        CONST(200), bare("BLOCK_END"),   // case 1 — normal
                        CONST(300), bare("RETURN"),      // case 2 — bare terminator (non-last)
                        CONST(400), bare("BLOCK_END"),   // case 3 — normal, last case
                        bare("RETURN"),
                    ],
                }],
            }
        }

        for(const [selector, want] of [[0, 100], [1, 200], [2, 300], [3, 400]] as const)
        {
            test(`selector ${selector} -> ${want}`, () =>
            {
                assert.equal(runOnQemu(checkedTranslate(program(selector)), 0), want)
            })
        }
    })

    describe("BR_TABLE N>2 (jump-table helper): the *last* case closes via RETURN", () =>
    {
        // `resolveCaseClose`'s own `table.endSlot` patch (reached once
        // `remaining` hits 0) exercised via `closeCaseViaTerminator`
        // specifically, not `closeBlockEnd` — the previous describe block
        // only ever closed a *non-last* case this way.
        //
        // The trailing `RETURN` is deliberately *bare* (no `CONST`
        // override): case 0 closes normally (`BLOCK_END`), and isa-core.md
        // §7.1 has that fall through to right here with whatever case 0
        // itself left in acc — a first draft of this test put a `CONST(0)`
        // ahead of it "since it's never reached for cases 1/2, which both
        // return directly," which missed that case 0's own fall-through
        // *does* reach it, and got clobbered to 0 instead of case 0's own
        // 10 as a result. Matches the established shared-tail pattern
        // br-table.test.ts's own `fourCaseProgram` already uses.
        function program(selector: number): RtlProgram
        {
            return {
                procedures: [{
                    argCount: 0,
                    body: [
                        CONST(selector),
                        brTable(3),
                        CONST(10), bare("BLOCK_END"),  // case 0 — normal, falls through to the shared tail below
                        CONST(20), bare("RETURN"),     // case 1 — bare terminator (non-last)
                        CONST(30), bare("RETURN"),     // case 2 — bare terminator, the construct's own last case
                        bare("RETURN"),                 // shared tail — reached only via case 0's own fall-through
                    ],
                }],
            }
        }

        for(const [selector, want] of [[0, 10], [1, 20], [2, 30]] as const)
        {
            test(`selector ${selector} -> ${want}`, () =>
            {
                assert.equal(runOnQemu(checkedTranslate(program(selector)), 0), want)
            })
        }
    })

    describe("a LOOP nested inside a case, its own body closed by RETURN, doesn't disturb the enclosing case's own bookkeeping", () =>
    {
        // Confirms closing an *inner* construct via a terminator never
        // needs to propagate any special handling to an *outer* one still
        // open around it: the outer case's own `Frame` is an untouched
        // local in a suspended, enclosing `translateBody` call — once the
        // inner LOOP's own recursive call returns (however it closed), the
        // outer case just keeps translating from wherever `pc` now points,
        // exactly as it would if the inner construct had closed normally.
        //
        // case 0's own body is *only* the LOOP — isa-core.md §7.2's own
        // "code after the loop is reached only via the condition's own
        // exit path" means the LOOP itself never closes case 0; case 0
        // still needs its own explicit `BLOCK_END` right after it (the
        // condition here is a constant 1, so that exit path is never
        // actually taken for any `n` below — dead in practice, but
        // validate.ts doesn't evaluate constants, so it's still required).
        function program(n: number): RtlProgram
        {
            return {
                procedures: [{
                    argCount: 0,
                    body: [
                        CONST(n), opImm("GT_U", 10), brTable(2),
                            // case 0 (n <= 10): a loop that always runs its
                            // body exactly once and returns from inside it.
                            CONST(1), bare("LOOP"), bare("BLOCK_END"),
                                CONST(77), bare("RETURN"),   // loop body — bare terminator
                            bare("BLOCK_END"),                // closes case 0 itself
                            // case 1 (n > 10): closes normally, and must
                            // still be reachable/correct after the above.
                            CONST(88), bare("BLOCK_END"),
                        bare("RETURN"),
                    ],
                }],
            }
        }

        for(const [n, want] of [[0, 77], [50, 88]] as const)
        {
            test(`n=${n} -> ${want}`, () =>
            {
                assert.equal(runOnQemu(checkedTranslate(program(n)), 0), want)
            })
        }
    })

    describe("window bookkeeping resets correctly across a terminator-closed case", () =>
    {
        // case 0 pushes a local (moving `window.tos` from this construct's
        // own entry depth) before returning early. If the compiler's own
        // `window.tos` bookkeeping weren't reset back to that entry depth
        // afterward, case 1's own `PUSH` would target the *wrong* physical
        // register (one slot further than intended) and its immediately
        // following `LOAD(0)` would read back whatever garbage was already
        // sitting in register 0 — never written on case 1's own execution
        // path — instead of the value case 1 itself just pushed.
        function program(n: number): RtlProgram
        {
            return {
                procedures: [{
                    argCount: 0,
                    body: [
                        CONST(n), opImm("GT_U", 10), brTable(2),
                            CONST(50), PUSH(), CONST(999), bare("RETURN"), // case 0 (n <= 10) — pushes a local, then returns early
                            CONST(1000), PUSH(), LOAD(0), bare("BLOCK_END"), // case 1 (n > 10) — its own, unrelated push+load of register 0
                        bare("RETURN"),
                    ],
                }],
            }
        }

        test("n=0 (case 0, terminator close)", () => { assert.equal(runOnQemu(checkedTranslate(program(0)), 0), 999) })
        test("n=50 (case 1, register 0 resolves to its own pushed 1000, not case 0's leftover bookkeeping)", () =>
        {
            assert.equal(runOnQemu(checkedTranslate(program(50)), 0), 1000)
        })
    })
})
