// fuzz — calibration for the unsequenced-effect filter.
//
//     npx ts-node --transpile-only fuzz/ts/gen/ub_check.ts
//
// Both directions matter equally. Missing a real conflict manufactures a
// mismatch that is not a bug; flagging a sequenced one throws away coverage
// the language actually defines.

import { ir } from "mog-core"
import { analyze } from "./ub"
import type { GenProgram } from "./corpus"

const one = (source: string, args: string[] = ["a", "b"]): GenProgram =>
    ({procs: [{args, body: [...ir`${source}`.body]}]})

/** Conflicting: the answer depends on which operand the lowerer runs first. */
const UNSEQUENCED: [string, string][] = [
    ["write racing a read", `u32 x = 1; return x + (x = 3);`],
    ["two writes to one name", `u32 x = 1; return (x = 1) + (x = 2);`],
    ["postfix update racing a read", `u32 x = 1; return x + x++;`],
    ["two updates", `u32 x = 1; return x++ + x++;`],
    ["memory read racing a write", `return ld32(0) + st32(4, 1);`],
    ["two memory writes", `return st32(0, 1) + st32(4, 2);`],
    ["unsequenced call arguments", `u32 x = 1; st32(x++, x); return x;`],
    ["compare racing a store", `return memcmp(0, 4, 8) + st32(0, 1);`],
    ["write under a cast", `u32 x = 1; return u8(x) + (x = 2);`],
    ["nested deeper", `u32 x = 1; return (1 + (2 * x)) - (x = 9);`],
]

/** Defined: something sequences the two, so both may have effects. */
const SEQUENCED: [string, string][] = [
    ["pure operands", `return a + b;`],
    ["two memory reads", `return ld32(0) + ld32(4);`],
    ["logical and", `u32 x = 1; return a && (x = 1);`],
    ["logical or", `u32 x = 1; return a || (x = 1);`],
    ["ternary arms", `u32 x = 1; return a ? x++ : x;`],
    ["ternary test against an arm", `u32 x = 1; return (x = 1) ? x : 0;`],
    ["statement boundary", `u32 x = 1; x = 2; return x + 1;`],
    ["assignment reading its own target", `u32 x = 1; x += a; return x;`],
    ["store then load", `st32(0, a); return ld32(0);`],
    ["one effect only", `u32 x = 1; return b + (x = 3);`],
]

let failures = 0

for(const [label, source] of UNSEQUENCED)
{
    const r = analyze(one(source))
    if(!r.unsequenced) { console.log(`MISSED   ${label.padEnd(30)} ${JSON.stringify(source)}`); failures++ }
}

for(const [label, source] of SEQUENCED)
{
    const r = analyze(one(source))
    if(r.unsequenced) { console.log(`FLAGGED  ${label.padEnd(30)} ${JSON.stringify(source)}   at ${r.site}`); failures++ }
}

// Across procedures: only the extension buffer crosses a call, since
// arguments are by value and the language has no pointers.
const crossProc: GenProgram = {procs: [
    {args: ["a"], body: [...ir`return p1() + p2();`.body]},
    {args: [], body: [...ir`st32(0, 1); return 1;`.body]},
    {args: [], body: [...ir`return ld32(0);`.body]},
]}
if(!analyze(crossProc).unsequenced) { console.log("MISSED   callee memory effects across two operands"); failures++ }

const crossProcPure: GenProgram = {procs: [
    {args: ["a"], body: [...ir`return p1() + p2();`.body]},
    {args: [], body: [...ir`return 1;`.body]},
    {args: [], body: [...ir`return 2;`.body]},
]}
if(analyze(crossProcPure).unsequenced) { console.log("FLAGGED  two pure callees"); failures++ }

console.log(failures === 0
    ? `ub: ${UNSEQUENCED.length + SEQUENCED.length + 2} cases, all correct`
    : `ub: ${failures} case(s) wrong`)
process.exit(failures === 0 ? 0 : 1)
