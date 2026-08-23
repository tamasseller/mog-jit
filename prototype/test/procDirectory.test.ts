/**
 * @ppl/jit-armv6m-prototype/test — procedure directory / skip-pass
 *
 * `@ppl/machine`'s `encodeProgram` only ever produces the wire bytes these
 * tests feed in — a test-fixture role, same as `br-table.test.ts`'s own
 * use of rtl.ts constructors, not the "own thing, not shared code" this
 * package's actual translation logic (bytecodeReader.ts/procDirectory.ts)
 * is built around.
 */

import { describe, test } from "node:test"
import assert from "node:assert/strict"

import { CONST, bare, brTable, call, encodeProgram } from "@ppl/machine"
import type { RtlProgram } from "@ppl/machine"
import { buildProcDirectory } from "../src/procDirectory"

describe("procDirectory", () =>
{
    test("two ordinary procedures — offsets, argCounts, savesLR=false for both", () =>
    {
        const program: RtlProgram = {
            procedures: [
                { argCount: 0, body: [CONST(5), bare("RETURN")] },
                { argCount: 2, body: [CONST(1), bare("RETURN")] },
            ],
        }
        const bytes = encodeProgram(program)
        const dir = buildProcDirectory(bytes)

        assert.equal(dir.length, 2)
        assert.equal(dir[0]!.argCount, 0)
        assert.equal(dir[0]!.savesLR, false)
        assert.equal(dir[1]!.argCount, 2)
        assert.equal(dir[1]!.savesLR, false)
        // proc 1's own body starts exactly where proc 0's ended — no gap,
        // no overlap, matching bytes[dir[0].bytecodeOffset..] having
        // consumed exactly proc 0's own body.
        assert.equal(bytes[dir[1]!.bytecodeOffset - 1], 2) // proc 1's own arg_count byte, right before its body
    })

    test("a CALL anywhere in the body sets savesLR", () =>
    {
        const program: RtlProgram = {
            procedures: [
                { argCount: 0, body: [call(1), bare("RETURN")] },
                { argCount: 0, body: [bare("RETURN")] },
            ],
        }
        const dir = buildProcDirectory(encodeProgram(program))
        assert.equal(dir[0]!.savesLR, true)
        assert.equal(dir[1]!.savesLR, false)
    })

    test("BR_TABLE N<=2 (branch fusion) does not set savesLR; N>2 (jump-table helper) does", () =>
    {
        const twoCase: RtlProgram = {
            procedures: [{ argCount: 0, body: [CONST(0), brTable(2), CONST(1), bare("BLOCK_END"), CONST(2), bare("BLOCK_END"), bare("RETURN")] }],
        }
        const fourCase: RtlProgram = {
            procedures: [{
                argCount: 0,
                body: [
                    CONST(0), brTable(4),
                    CONST(1), bare("BLOCK_END"), CONST(2), bare("BLOCK_END"),
                    CONST(3), bare("BLOCK_END"), CONST(4), bare("BLOCK_END"),
                    bare("RETURN"),
                ],
            }],
        }
        assert.equal(buildProcDirectory(encodeProgram(twoCase))[0]!.savesLR, false)
        assert.equal(buildProcDirectory(encodeProgram(fourCase))[0]!.savesLR, true)
    })

    test("NEG/NOT (single native instructions) don't set savesLR; CLZ/REVBITS (software helpers, reached by a local BL) do", () =>
    {
        const negOnly: RtlProgram = { procedures: [{ argCount: 0, body: [CONST(5), bare("NEG"), bare("RETURN")] }] }
        const notOnly: RtlProgram = { procedures: [{ argCount: 0, body: [CONST(5), bare("NOT"), bare("RETURN")] }] }
        const clz: RtlProgram = { procedures: [{ argCount: 0, body: [CONST(5), bare("CLZ"), bare("RETURN")] }] }
        const revbits: RtlProgram = { procedures: [{ argCount: 0, body: [CONST(5), bare("REVBITS"), bare("RETURN")] }] }

        assert.equal(buildProcDirectory(encodeProgram(negOnly))[0]!.savesLR, false)
        assert.equal(buildProcDirectory(encodeProgram(notOnly))[0]!.savesLR, false)
        assert.equal(buildProcDirectory(encodeProgram(clz))[0]!.savesLR, true)
        assert.equal(buildProcDirectory(encodeProgram(revbits))[0]!.savesLR, true)
    })

    test("a LOOP body closed by a bare terminator (isa-core.md §7.2) still finds the right boundary", () =>
    {
        // The exact shape loop.test.ts pins down at the translator level,
        // and bytecode.test.ts at the wire-decode level — here, the
        // skip-pass must not mistake the loop's own bare-terminator closer
        // for the whole procedure's end, or proc 1 below would be read
        // starting from the wrong offset entirely.
        const program: RtlProgram = {
            procedures: [
                {
                    argCount: 0,
                    body: [
                        CONST(1), bare("LOOP"), bare("BLOCK_END"),
                        CONST(42), bare("RETURN"), // closes the loop body, not the procedure
                        CONST(0), bare("RETURN"),  // the outer scope's own tail
                    ],
                },
                { argCount: 3, body: [call(0), bare("RETURN")] }, // savesLR=true, to also prove decode kept going correctly
            ],
        }
        const dir = buildProcDirectory(encodeProgram(program))
        assert.equal(dir.length, 2)
        assert.equal(dir[0]!.savesLR, false)
        assert.equal(dir[1]!.argCount, 3)
        assert.equal(dir[1]!.savesLR, true)
    })
})
