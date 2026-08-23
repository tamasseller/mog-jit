/**
 * @ppl/jit-armv6m-prototype/test — `bytecodeReader.ts`'s `decodeInstr`
 * (docs/design.md §16 item 16)
 *
 * Cross-checks this package's own lean decoder against `@ppl/machine`'s
 * `encodeInstr` (the wire format's own reference implementation) for one
 * instruction of every shape `decodeInstr` claims to handle — the same
 * discipline as `procDirectory.test.ts`'s own use of `encodeProgram` as a
 * test fixture, not shared translation logic.
 */

import { describe, test } from "node:test"
import assert from "node:assert/strict"

import {
    encodeInstr,
    opReg, opRegWriteback, opImm, opStack, bare, LOAD, STORE, CONST, PUSH, POP,
    brTable, trap, call,
} from "@ppl/machine"
import type { RtlInstr } from "@ppl/machine"
import { decodeInstr } from "../src/bytecodeReader"

const CASES: readonly RtlInstr[] = [
    opReg("ADD", 3), opRegWriteback("SUB", 1), opStack("MUL", "PEEK_PEEK"), opStack("AND", "POP_ACC"), opImm("XOR", 42),
    opReg("EQ", 2), opStack("LT_S", "POP_ACC"), opImm("EQ", 0), opImm("GE_U", 100),
    bare("NEG"), bare("NOT"), bare("CLZ"), bare("REVBITS"),
    bare("BLOCK_END"), bare("LOOP"), brTable(1), brTable(2), brTable(5),
    call(3), bare("RETURN"), trap(0), trap(7),
    PUSH(), POP(), LOAD(4), STORE(9), CONST(3), CONST(12345),
]

describe("bytecodeReader.decodeInstr (§16 item 16)", () =>
{
    for(const instr of CASES)
    {
        test(`round-trips ${JSON.stringify(instr)}`, () =>
        {
            const bytes = Uint8Array.from(encodeInstr(instr))
            const { instr: decoded, next } = decodeInstr(bytes, 0)
            assert.deepEqual(decoded, instr)
            assert.equal(next, bytes.length)
        })
    }

    test("decodes a run of several instructions back to back, advancing `next` each time", () =>
    {
        const program = [opImm("ADD", 5), LOAD(0), bare("RETURN")]
        const bytes = Uint8Array.from(program.flatMap(i => encodeInstr(i)))
        let pos = 0
        for(const expected of program)
        {
            const { instr, next } = decodeInstr(bytes, pos)
            assert.deepEqual(instr, expected)
            pos = next
        }
        assert.equal(pos, bytes.length)
    })
})
