// fuzz — what the generator mutates.
//
// A corpus entry is a *procedure graph*, not one AST: the grammar has no
// function-declaration node (ast.ts's `Program` is a bare `Statement[]`),
// so a multi-procedure program is composed at this level and stitched
// together through each fragment's own `calls` map, which is what
// `lowerProgram` resolves a callee name through (lower.ts).
//
// Procedure `i` is named `p<i>` and may only call a strictly higher index.
// That is the call graph's acyclicity (isa-core.md §8.2) held as a
// representation invariant rather than discovered by the validator, and it
// is also why the graph needs no separate edge list: the calls in the tree
// are the edges.

import { lowerProgram, proc } from "mog-core"
import { ir } from "mog-core"
import type { IrFragment, Procedure, RtlProgram, Statement } from "mog-core"
import type { Extension } from "mog-core"
import { roundTrip } from "./round_trip"
import { print } from "./print"
import { eachExpression } from "./walk"

export interface GenProc
{
    /** Parameter names in order. Untyped, so every one is `u32` —
     *  isa-core.md §2.3 gives a procedure argument no other choice. */
    args: string[]
    body: Statement[]
}

export interface GenProgram
{
    /** Entry is index 0. */
    procs: GenProc[]
}

const NAME = /^p([0-9]+)$/

export const procName = (index: number): string => `p${index}`

/** The indices `body` calls, read out of the tree itself. */
export function calleesOf(body: readonly Statement[]): number[]
{
    const found = new Set<number>()
    eachExpression(body, e =>
    {
        if(e.type !== "CallExpression") return
        const m = NAME.exec(e.callee.name)
        if(m !== null) found.add(Number(m[1]))
    })
    return [...found].sort((a, b) => a - b)
}

export class GraphError extends Error {}

/** Every structural invariant the representation promises, checked in one
 *  place so a generator bug surfaces here rather than as a mysterious
 *  lowering failure several stages later. */
export function checkGraph(gen: GenProgram): void
{
    if(gen.procs.length === 0) throw new GraphError("no entry procedure")

    gen.procs.forEach((p, i) =>
    {
        for(const callee of calleesOf(p.body))
        {
            if(callee >= gen.procs.length) throw new GraphError(`p${i} calls p${callee}, which does not exist`)
            if(callee <= i) throw new GraphError(`p${i} calls p${callee} — a call must target a higher index, or the graph has a cycle`)
        }
    })
}

/** Build the `Procedure` graph. Built from the last index down, since a call
 *  only ever points upward and a fragment's `calls` map needs its callees
 *  already constructed.
 *
 *  `verify` prints each body and reparses it, so the tree that reaches the
 *  lowerer is one the real parser produced and `print`/`parse` are checked
 *  against each other. That is worth doing — but the PEG parser is, by
 *  measurement, essentially the whole cost of the inner loop, so a campaign
 *  checks the property over its corpus on a time budget instead of paying it
 *  per candidate. Unverified, the mutator's own tree is handed over directly
 *  and `source` is printed only if something asks for it (an error
 *  message).  */
export function toProcedures(gen: GenProgram, verify = true): Procedure
{
    checkGraph(gen)

    const built: Procedure[] = new Array(gen.procs.length)

    for(let i = gen.procs.length - 1; i >= 0; i--)
    {
        const p = gen.procs[i]!
        const calls = new Map<string, Procedure>()
        for(const callee of calleesOf(p.body)) calls.set(procName(callee), built[callee]!)

        // Assembled rather than produced by `ir\`...\`` itself: the callee
        // names are this module's own, not the synthetic ones a splice mints.
        const fragment: IrFragment = verify
            ? { type: "IrFragment", ...roundTrip(p.body), calls }
            : { type: "IrFragment", body: p.body, calls, get source() { return print(p.body) } }

        built[i] = proc(p.args, fragment)
    }

    return built[0]!
}

export function lowerGen<E extends {ext: string}>(
    gen: GenProgram, extension?: Extension<E>, verify = true): RtlProgram<E>
{
    return lowerProgram(toProcedures(gen, verify), extension)
}

// ── seeds ───────────────────────────────────────────────────────────────
//
// Authored as source, because that is what a person reads and edits. Every
// one is parsed at load, so a seed that does not even parse fails here
// rather than becoming a corpus entry that silently never mutates into
// anything.

interface SeedSpec
{
    name: string
    procs: {args?: string[]; source: string}[]
}

const SEEDS: SeedSpec[] = [
    {name: "const", procs: [{source: `return 37;`}]},
    {name: "arith", procs: [{args: ["a", "b"], source: `return (a + b) * 3 - (a ^ b);`}]},
    {name: "shift", procs: [{args: ["a"], source: `return (a << 3) | (a >> 2);`}]},
    {name: "compare", procs: [{args: ["a", "b"], source: `return (a < b) + (a == b) + (a >= b);`}]},
    {name: "types", procs: [{args: ["a"], source: `i16 x = i16(a); u8 y = u8(a); return x + y;`}]},
    // Signedness only shows when both operands promote to `i32` and one
    // holds a high bit — a narrow variable stored already sign-extended is
    // the reachable way there, and without this seed no mutant ever ran a
    // signed compare whose answer differed from the unsigned one.
    {name: "signed", procs: [{args: ["a"], source:
        `i8 p = i8(a | 0x80); i16 q = i16(a | 0x8000); i32 r = 0 - a;`
        + ` return (p < 0) + (q < 1) + (r <= 0) + (p >> 2) + (r >> 4);`}]},
    {name: "ternary", procs: [{args: ["a"], source: `return a ? a + 1 : a - 1;`}]},
    {name: "logical", procs: [{args: ["a", "b"], source: `return (a && b) || (a > b);`}]},
    // Each update is its own statement: two in one expression would be
    // unsequenced against each other and dropped before any engine ran it.
    {name: "update", procs: [{args: ["a"], source:
        `u32 i = a; u32 j = i++; u32 k = ++i; return i + j + k;`}]},

    {name: "if", procs: [{args: ["a"], source: `if (a > 3) { return 1; } else { return 2; }`}]},
    {name: "nested_if", procs: [{args: ["a", "b"], source:
        `if (a) { if (b) { return 1; } else { return 2; } } else { return 3; }`}]},
    {name: "while", procs: [{args: ["a"], source:
        `u32 n = a & 7; u32 s = 0; while (n) { s = s + n; n = n - 1; } return s;`}]},
    {name: "dowhile", procs: [{args: ["a"], source:
        `u32 n = a & 3; u32 s = 0; do { s = s + 1; n = n - 1; } while (n); return s;`}]},
    {name: "for", procs: [{args: ["a"], source:
        `u32 s = 0; for (u32 i = 0; i < (a & 7); i = i + 1) { s = s + i; } return s;`}]},
    {name: "nested_loop", procs: [{args: ["a"], source:
        `u32 s = 0; for (u32 i = 0; i < 3; i = i + 1) { for (u32 j = 0; j < 3; j = j + 1) { s = s + i * j; } } return s + a;`}]},
    {name: "switch", procs: [{args: ["a"], source:
        `switch (a & 3) { case 0: return 10; case 1: return 11; case 2: return 12; default: return 13; }`}]},
    {name: "switch_break", procs: [{args: ["a"], source:
        `u32 s = 0; switch (a & 3) { case 0: s = 1; break; case 1: s = 2; break; default: s = 9; break; } return s;`}]},
    {name: "block_scope", procs: [{args: ["a"], source: `u32 s = a; { u32 t = 2; s = s + t; } return s;`}]},
    {name: "trap", procs: [{args: ["a"], source: `if (a == 0) { trap(7); } return a;`}]},
    {name: "builtins", procs: [{args: ["a"], source: `return clz(a) + revbits(a);`}]},

    {name: "call_none", procs: [
        {source: `return p1() + 1;`},
        {source: `return 7;`},
    ]},
    {name: "call_one", procs: [
        {args: ["a"], source: `return p1(a);`},
        {args: ["x"], source: `return x * 3;`},
    ]},
    {name: "call_four", procs: [
        {args: ["a"], source: `return p1(a, a + 1, a + 2, a + 3);`},
        {args: ["w", "x", "y", "z"], source: `return w + x + y + z;`},
    ]},
    {name: "call_six", procs: [
        {args: ["a"], source: `return p1(a, 1, 2, 3, 4, 5);`},
        {args: ["u", "v", "w", "x", "y", "z"], source: `return u + v + w + x + y + z;`},
    ]},
    {name: "call_chain", procs: [
        {args: ["a"], source: `return p1(a) + p2(a);`},
        {args: ["x"], source: `return p2(x) + 1;`},
        {args: ["x"], source: `return x * 2;`},
    ]},
    {name: "call_in_branch", procs: [
        {args: ["a"], source: `if (a & 1) { return p1(a); } return p1(a + 1);`},
        {args: ["x"], source: `return x ^ 5;`},
    ]},

    {name: "ext_load_store", procs: [{args: ["a"], source:
        `st32(0x10, a); st16(0x20, a); st8(0x30, a); return ld32(0x10) + ld16(0x20) + ld8(0x30);`}]},
    {name: "ext_memmove", procs: [{args: ["a"], source:
        `st32(0x10, a); memmove(0x10, 0x12, 0x1a); return ld32(0x14);`}]},
    {name: "ext_memcmp", procs: [{args: ["a"], source:
        `st32(0x50, a); st32(0x60, a); return memcmp(0x50, 0x54, 0x60);`}]},
    {name: "ext_slicecmp", procs: [{args: ["a"], source:
        `st32(0x80, a); st32(0x90, a); return slicecmp(0x80, 0x84, 0x90, 0x94);`}]},
    {name: "ext_in_loop", procs: [{args: ["a"], source:
        `for (u32 i = 0; i < 4; i = i + 1) { st8(0x100 + i, a + i); } return ld32(0x100);`}]},
    // The call is bound to a local first: a callee that writes the buffer
    // racing a load of it in the other operand is exactly the unsequenced
    // shape ub.ts drops.
    {name: "ext_across_call", procs: [
        {args: ["a"], source: `st32(0x10, a); u32 t = p1(); return t + ld32(0x10);`},
        {source: `st32(0x14, 9); return ld32(0x14);`},
    ]},
]

export function seedCorpus(): {name: string; program: GenProgram}[]
{
    return SEEDS.map(spec =>
    {
        const program: GenProgram = {
            procs: spec.procs.map(p => ({args: p.args ?? [], body: [...ir`${p.source}`.body]})),
        }
        try { checkGraph(program) }
        catch(e) { throw new Error(`seed "${spec.name}": ${(e as Error).message}`) }
        return {name: spec.name, program}
    })
}
