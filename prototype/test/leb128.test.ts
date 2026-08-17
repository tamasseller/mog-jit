/**
 * @ppl/jit-armv6m-prototype/test — leb128_len smoke test
 *
 * The first real signal on the whole pipeline: DSL source → lowerProc
 * (@ppl/machine, real bytecode) → translateProc (this package's
 * translator) → real Thumb machine code → real QEMU execution, compared
 * against a plain-JS reference. Deliberately the same algorithm as
 * isa-core.md's own worked example (and docs/jit-armv6m.md's Appendix hand
 * translation of it) — but built through the real DSL/lowerer rather than
 * hand-copying that example's bytecode listing, since (as this session's
 * notes recorded) that listing's `STORE`-without-a-preceding-`PUSH` shape
 * doesn't hold up against this codebase's own validate.ts/vm.ts semantics
 * (a local's index must already be TOS-covered). Exercises §5's window
 * mapping (`v`/`n` share the 4-deep window with no room to spare) and
 * §10.1's full fusion state machine, but not §6 (`CALL`) — this procedure
 * has none, matching translateProc.ts's current scope.
 */

import { describe, test } from "node:test"
import assert from "node:assert/strict"

import { ir, lowerProc, validateProgram } from "@ppl/machine"
import { translateProc } from "../src/translateProc"
import { runOnQemu } from "./qemu-run"

function leb128LenReference(v: number): number
{
    let n = 1
    v = v >>> 0
    while(v >= 0x80) { v = v >>> 7; n++ }
    return n
}

describe("leb128_len", () =>
{
    const frag = ir`
        u32 n = 1;
        while (v >= 0x80)
        {
            v = v >> 7;
            n = n + 1;
        }
        return n;
    `
    const proc = lowerProc(frag.body, ["v"])
    validateProgram({ procedures: [proc] })
    const { code } = translateProc(proc)

    test("translates without throwing", () =>
    {
        assert.ok(code.length > 0)
    })

    for(const v of [0, 1, 0x7f, 0x80, 0x81, 0x3fff, 0x4000, 0x1fffff, 0x200000, 0xffffffff, 0x12345678])
    {
        test(`leb128_len(0x${v.toString(16)}) on real QEMU`, () =>
        {
            const got = runOnQemu(code, v)
            const want = leb128LenReference(v)
            assert.equal(got, want, `leb128_len(0x${v.toString(16)}): expected ${want}, got ${got}`)
        })
    }
})
