/**
 * @ppl/jit-armv6m-prototype/test — unary ops + comparisons as ordinary
 * values (docs/design.md §16 item 8)
 *
 * `translateProc.ts` used to throw on `NEG`/`NOT`/`CLZ`/`REVBITS`
 * entirely, and always treated a comparison as feeding a following
 * `BR_TABLE`/`LOOP` condition — but the DSL already lowers `return a == b;`
 * or `(n > 4) * 5` as ordinary value-producing expressions (isa-core.md
 * §4.2, `vm.ts`'s own `evalBinary`/`evalUnary`), so this translator's
 * fusion-only/throw-always behavior was the gap, not the ISA. Covers:
 * `NEG`/`NOT` (single native instructions), `CLZ`/`REVBITS` (the new
 * per-procedure software helpers, `unaryops.ts`), a comparison
 * materialized as a plain value, one folded into a following `STORE`, one
 * feeding further arithmetic (`(n > k) * 5`), and — the actual fusion/
 * materialize *gating* logic itself — both shapes coexisting in one
 * procedure.
 *
 * Hand-built via @ppl/machine's own rtl.ts constructors (same discipline
 * as comparison-fusion.test.ts) — needs exact control over what
 * immediately follows a comparison, which the `ir` DSL's own lowering
 * choices would obscure.
 */

import { describe, test } from "node:test"
import assert from "node:assert/strict"

import { CONST, PUSH, LOAD, STORE, opImm, bare, brTable, validateProgram, run } from "@ppl/machine"
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

function oneInstrProgram(imm: number, op: "NEG" | "NOT" | "CLZ" | "REVBITS"): RtlProgram
{
    return { procedures: [{ argCount: 0, body: [CONST(imm), bare(op), bare("RETURN")] }] }
}

describe("unary ops (§16 item 8)", () =>
{
    // `runOnQemu`'s own return convention (qemu-run.ts) reads bit 31 of
    // the final result as "this procedure TRAPped" — a real value with
    // that bit set (any negative-looking NEG/NOT/REVBITS result) would be
    // misread as a trap, not a translator bug. Every case below folds the
    // unary op's result through one more, harness-safe operation so the
    // *final* RETURN value never has bit 31 set, while still needing the
    // unary op itself to have run correctly to land on the expected number.

    test("NEG", () =>
    {
        const program: RtlProgram = {
            procedures: [{ argCount: 0, body: [CONST(5), bare("NEG"), opImm("ADD", 10), bare("RETURN")] }],
        }
        const code = checkedTranslate(program)
        assert.equal(runOnQemu(code, 0), 5) // -5 + 10
    })

    test("NOT", () =>
    {
        const program: RtlProgram = {
            procedures: [{ argCount: 0, body: [CONST(5), bare("NOT"), opImm("AND", 0xff), bare("RETURN")] }],
        }
        const code = checkedTranslate(program)
        assert.equal(runOnQemu(code, 0), (~5) & 0xff) // 0xfa
    })

    describe("CLZ", () =>
    {
        for(const [value, want] of [[0, 32], [1, 31], [0x80000000, 0], [0xff, 24], [0x0000ffff, 16]] as const)
        {
            test(`clz(0x${value.toString(16)}) === ${want}`, () =>
            {
                const code = checkedTranslate(oneInstrProgram(value, "CLZ"))
                assert.equal(runOnQemu(code, 0), want)
            })
        }
    })

    describe("REVBITS", () =>
    {
        // Chosen so the reversed result itself never has bit 31 set.
        for(const [value, want] of [[0x80000000, 1], [0xff000000, 0xff], [0, 0], [2, 0x40000000]] as const)
        {
            test(`revbits(0x${value.toString(16)}) === 0x${want.toString(16)}`, () =>
            {
                const code = checkedTranslate(oneInstrProgram(value, "REVBITS"))
                assert.equal(runOnQemu(code, 0), want)
            })
        }
    })

    test("NEG folds into a following STORE (destination-fold, same as any other producer)", () =>
    {
        const program: RtlProgram = {
            procedures: [{
                argCount: 0,
                body: [CONST(0), PUSH(), CONST(7), bare("NEG"), STORE(0), LOAD(0), opImm("ADD", 100), bare("RETURN")],
            }],
        }
        const code = checkedTranslate(program)
        assert.equal(runOnQemu(code, 0), 93) // -7 + 100
    })
})

describe("comparisons as ordinary values (§16 item 8)", () =>
{
    test("a bare comparison, materialized directly (return a == b)", () =>
    {
        const program: RtlProgram = {
            procedures: [{ argCount: 0, body: [CONST(2), opImm("EQ", 2), bare("RETURN")] }],
        }
        const code = checkedTranslate(program)
        assert.equal(runOnQemu(code, 0), 1)
    })

    test("a comparison folds into a following STORE", () =>
    {
        const program: RtlProgram = {
            procedures: [{
                argCount: 0,
                body: [CONST(3), PUSH(), LOAD(0), opImm("EQ", 3), STORE(0), LOAD(0), bare("RETURN")],
            }],
        }
        const code = checkedTranslate(program)
        assert.equal(runOnQemu(code, 0), 1)
    })

    describe("a comparison feeds further arithmetic: (n > 4) * 5", () =>
    {
        function program(n: number): RtlProgram
        {
            return {
                procedures: [{
                    argCount: 0,
                    body: [CONST(n), opImm("GT_U", 4), opImm("MUL", 5), bare("RETURN")],
                }],
            }
        }

        for(const [n, want] of [[6, 5], [3, 0], [4, 0], [5, 5]] as const)
        {
            test(`n=${n} -> ${want}`, () =>
            {
                const code = checkedTranslate(program(n))
                assert.equal(runOnQemu(code, 0), want)
            })
        }
    })

    describe("fusion and materialization coexist in one procedure", () =>
    {
        // if (n > 10): return (n == 20);   else: return 0;
        // Exercises the *gating* logic itself — one comparison fuses into
        // BR_TABLE (unaffected by this change), a second, later comparison
        // in the very same body is materialized as a value instead.
        function program(n: number): RtlProgram
        {
            return {
                procedures: [{
                    argCount: 0,
                    body: [
                        CONST(n), opImm("GT_U", 10), brTable(2),
                            CONST(0), bare("BLOCK_END"),                    // case 0 (n <= 10): 0
                            CONST(n), opImm("EQ", 20), bare("BLOCK_END"),     // case 1 (n > 10): materialized (n == 20)
                        bare("RETURN"),                                        // merge point
                    ],
                }],
            }
        }

        for(const [n, want] of [[5, 0], [20, 1], [15, 0]] as const)
        {
            test(`n=${n} -> ${want}`, () =>
            {
                const code = checkedTranslate(program(n))
                assert.equal(runOnQemu(code, 0), want)
            })
        }
    })
})
