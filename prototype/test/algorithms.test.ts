/**
 * @ppl/jit-armv6m-prototype/test — core-testsuite algorithms on real QEMU
 *
 * Same four DSL programs as @ppl/machine's own algorithms.test.ts (Collatz,
 * Stein's binary GCD, bitwise isqrt, Russian peasant multiplication) —
 * chosen there for exercising a different loop/branch shape each, which is
 * exactly what's wanted here too. Unlike leb128.test.ts, every local here
 * is declared inside the DSL body (`argCount === 0`, the value embedded as
 * a source literal, matching algorithms.test.ts's own pattern) — several
 * of these push past the 4-register window with 3-4 live locals at once,
 * so unlike leb128_len this corpus actually exercises window.ts's
 * spill/fill path, not just its no-op common case.
 *
 * A representative sample of each sweep, not the full range
 * algorithms.test.ts itself uses (2000/900/3000/1600 cases) — each case
 * here is a real `make` + `qemu-system-arm` boot, so the full sweep would
 * take minutes; this still covers small values, boundary values, and
 * multi-digit values for each.
 */

import { describe, test } from "node:test"
import assert from "node:assert/strict"

import { ir, lowerProc, validateProgram } from "@ppl/machine"
import { translateProc } from "../src/translateProc"
import { runOnQemu } from "./qemu-run"

function translate(source: string): Uint16Array
{
    const frag = ir`${source}`
    const proc = lowerProc(frag.body)
    validateProgram({ procedures: [proc] })
    return translateProc(proc).code
}

describe("core-testsuite algorithms on real QEMU", () =>
{
    describe("Collatz: steps to reach 1", () =>
    {
        function reference(n: number): number
        {
            let steps = 0
            while(n !== 1)
            {
                n = (n & 1) === 0 ? n / 2 : (n * 3 + 1) >>> 0
                steps++
            }
            return steps
        }

        for(const n of [1, 2, 3, 7, 27, 97, 703, 1000, 2000, 6171])
        {
            test(`collatz(${n})`, () =>
            {
                const code = translate(`
                    u32 n = ${n};
                    u32 steps = 0;
                    while (n != 1)
                    {
                        if ((n & 1) == 0)
                            n = n >> 1;
                        else
                            n = n * 3 + 1;
                        steps = steps + 1;
                    }
                    return steps;
                `)
                const got = runOnQemu(code)
                assert.equal(got, reference(n), `collatz(${n})`)
            })
        }
    })

    describe("Stein's algorithm (binary GCD)", () =>
    {
        function reference(a: number, b: number): number
        {
            let shift = 0
            while(((a | b) & 1) === 0) { a >>>= 1; b >>>= 1; shift++ }
            while((a & 1) === 0) a >>>= 1
            while(b !== 0)
            {
                while((b & 1) === 0) b >>>= 1
                if(a > b) { const t = a; a = b; b = t }
                b -= a
            }
            return a << shift
        }

        for(const [a, b] of [[1, 1], [2, 4], [7, 13], [12, 18], [30, 30], [17, 29], [24, 36], [1, 30]] as const)
        {
            test(`gcd(${a},${b})`, () =>
            {
                const code = translate(`
                    u32 a = ${a};
                    u32 b = ${b};
                    u32 shift = 0;
                    while (((a | b) & 1) == 0)
                    {
                        a = a >> 1;
                        b = b >> 1;
                        shift = shift + 1;
                    }
                    while ((a & 1) == 0)
                        a = a >> 1;
                    while (b != 0)
                    {
                        while ((b & 1) == 0)
                            b = b >> 1;
                        if (a > b)
                        {
                            u32 t = a;
                            a = b;
                            b = t;
                        }
                        b = b - a;
                    }
                    return a << shift;
                `)
                const got = runOnQemu(code)
                assert.equal(got, reference(a, b), `gcd(${a},${b})`)
            })
        }
    })

    describe("Bitwise integer square root", () =>
    {
        function reference(n: number): number
        {
            return Math.floor(Math.sqrt(n))
        }

        for(const n of [0, 1, 2, 3, 15, 16, 1000, 32767, 32768, 65535, 65536, 1 << 30, 4294836224, 4294967295])
        {
            test(`isqrt(${n})`, () =>
            {
                const code = translate(`
                    u32 n = ${n};
                    u32 res = 0;
                    u32 bit = 1 << 30;
                    while (bit > n)
                        bit = bit >> 2;
                    while (bit != 0)
                    {
                        if (n >= res + bit)
                        {
                            n = n - (res + bit);
                            res = res + (bit << 1);
                        }
                        res = res >> 1;
                        bit = bit >> 2;
                    }
                    return res;
                `)
                const got = runOnQemu(code)
                assert.equal(got, reference(n), `isqrt(${n})`)
            })
        }
    })

    describe("Russian peasant multiplication", () =>
    {
        for(const [a, b] of [[0, 0], [1, 1], [5, 6], [13, 17], [100, 100], [200, 200], [0, 200], [200, 0]] as const)
        {
            test(`${a} * ${b}`, () =>
            {
                const code = translate(`
                    u32 a = ${a};
                    u32 b = ${b};
                    u32 result = 0;
                    while (a != 0)
                    {
                        if ((a & 1) != 0)
                            result = result + b;
                        a = a >> 1;
                        b = b << 1;
                    }
                    return result;
                `)
                const got = runOnQemu(code)
                assert.equal(got, a * b, `${a} * ${b}`)
            })
        }
    })
})
