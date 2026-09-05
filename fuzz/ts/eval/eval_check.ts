// fuzz — the evaluator's own gate: does it agree with the reference VM on
// semantics chosen to be awkward rather than random?
//
//     npx ts-node --transpile-only fuzz/ts/eval/eval_check.ts
//
// Random mutants turn out not to reach several of these at all — signed
// comparison in particular needs a narrow or `i32` variable holding a
// high-bit value against a small literal, which is a narrow target to hit
// by chance. Sabotaging a rule in ast-eval.ts must make this fail; if it
// does not, the case below is not doing what it claims.

import { validateProgram, run, UnspecifiedShiftAmount } from "mog-core"
import { ir } from "mog-core"
import { rawMemExtension } from "../lib/rawmem_ext"
import { lowerGen } from "../gen/corpus"
import { entryArgsFor } from "../lib/entry_args"
import type { GenProgram } from "../gen/corpus"
import { evaluate } from "./ast-eval"

const EXT = rawMemExtension()

const CASES: [string, string][] = [
    // Signedness — the rule is "unsigned iff an operand promotes to u32".
    ["i8 negative vs small literal", `i8 x = 255; return x < 5;`],
    ["i16 negative vs small literal", `i16 x = 65535; return x < 5;`],
    ["i32 high bit vs small literal", `i32 x = 0x80000000; return x < 5;`],
    ["u32 high bit stays unsigned", `u32 x = 0x80000000; return x < 5;`],
    ["mixed u32 makes it unsigned", `i32 x = 0x80000000; u32 y = 5; return x < y;`],
    ["large literal makes it unsigned", `i32 x = 3; return x < 0x80000000;`],
    ["signed >=", `i8 x = 255; return x >= 0;`],
    ["signed >", `i16 x = 65535; return x > 1;`],
    ["signed <=", `i8 x = 128; return x <= 0;`],

    // Shifts take signedness from the left operand alone.
    ["arithmetic shift right", `i32 x = 0x80000000; return x >> 4;`],
    ["logical shift right", `u32 x = 0x80000000; return x >> 4;`],
    ["narrow signed shift right", `i8 x = 255; return x >> 1;`],
    ["shift left is sign agnostic", `i32 x = 0x40000000; return x << 1;`],
    ["shift by 31", `u32 x = 1; return x << 31;`],
    ["dynamic shift by 32 is unspecified", `u32 n = 32; u32 x = 1; return x << n;`],
    ["dynamic shift by 31 is fine", `u32 n = 31; u32 x = 1; return x << n;`],

    // Narrow storage is kept already extended.
    ["u8 wraps on store", `u8 x = 300; return x;`],
    ["i8 sign extends on store", `i8 x = 200; return x;`],
    ["i16 sign extends on store", `i16 x = 40000; return x;`],
    ["u16 truncates", `u16 x = 70000; return x;`],
    ["narrow assignment re-narrows", `u8 x = 1; x = 511; return x;`],
    ["narrow update wraps", `u8 x = 255; x++; return x;`],
    ["narrow update underflows", `u8 x = 0; x--; return x;`],
    ["compound assignment narrows", `u8 x = 200; x += 100; return x;`],
    ["cast is not storage", `u32 a = 300; return u8(a) + a;`],

    // Wraparound.
    ["add wraps", `u32 x = 0xffffffff; return x + 2;`],
    // Observed through a comparison: returning the sum normalises it anyway,
    // so the wrap has to matter before it leaves the expression.
    ["add wraps before a compare", `u32 x = 0xffffffff; return (x + 2) < 5;`],
    ["subtract wraps before a compare", `u32 x = 1; return (x - 3) < 5;`],
    ["multiply wraps past float precision", `u32 x = 0xffffffff; return x * x;`],
    ["multiply wraps at the word", `u32 x = 0x10001; return x * x;`],
    ["negate wraps", `u32 x = 1; return -x;`],
    ["complement", `u32 x = 0; return ~x;`],
    ["not", `u32 x = 7; return !x;`],

    // An update's *value*, which differs from its effect.
    ["postfix yields the old value", `u32 x = 5; u32 y = x++; return y;`],
    ["prefix yields the new value", `u32 x = 5; u32 y = ++x; return y;`],
    ["postfix effect still lands", `u32 x = 5; u32 y = x++; return x;`],
    ["narrow postfix wraps and yields old", `u8 x = 255; u8 y = x++; return y * 2 + x;`],

    // A logical operator's value is a boolean, not either operand.
    ["and yields one, not the operand", `return 2 && 4;`],
    ["or yields one, not the operand", `return 0 || 7;`],
    ["and yields zero", `return 0 && 4;`],
    ["not of non-zero", `return !9;`],

    // Short circuit and ternary.
    ["and short circuits", `u32 x = 0; u32 y = 0; if (x && (y = 1)) { } return y;`],
    ["or short circuits", `u32 x = 1; u32 y = 0; if (x || (y = 1)) { } return y;`],
    ["ternary picks one arm", `u32 x = 0; u32 y = 5; return x ? (y = 1) : (y = 2);`],

    // Switch, including fall-through in source order.
    ["switch match", `u32 x = 1; switch (x) { case 0: return 10; case 1: return 11; default: return 12; }`],
    ["switch default", `u32 x = 9; switch (x) { case 0: return 10; case 1: return 11; default: return 12; }`],
    ["switch falls through", `u32 s = 0; switch (1) { case 1: s = s + 1; case 2: s = s + 2; break; default: break; } return s;`],
    ["switch break stops", `u32 s = 0; switch (1) { case 1: s = s + 1; break; case 2: s = s + 2; break; } return s;`],
    ["switch no match no default", `u32 s = 7; switch (9) { case 1: s = 1; break; } return s;`],

    // Loops.
    ["for accumulates", `u32 s = 0; for (u32 i = 0; i < 5; i = i + 1) { s = s + i; } return s;`],
    ["while counts down", `u32 n = 5; u32 s = 0; while (n) { s = s + n; n = n - 1; } return s;`],
    ["do runs once", `u32 n = 0; u32 s = 0; do { s = s + 1; } while (n); return s;`],
    // `break` closes a switch case and nothing else — there is no opcode
    // for irregular loop exit (isa-core.md §4.5, §10.3).
    ["break closes a case inside a loop", `u32 s = 0; for (u32 i = 0; i < 3; i = i + 1) { switch (i) { case 0: s = s + 1; break; default: s = s + 10; break; } } return s;`],

    // Builtins and the extension.
    ["clz", `u32 x = 1; return clz(x);`],
    ["clz of zero", `u32 x = 0; return clz(x);`],
    ["revbits", `u32 x = 1; return revbits(x);`],
    ["store then load", `st32(0x10, 0x01020304); return ld32(0x10);`],
    ["narrow store widths", `st8(0x20, 0xab); st16(0x30, 0xbeef); return ld8(0x20) + ld16(0x30);`],
    ["address masking", `st32(0x3fc, 7); return ld32(0x3fc);`],
    ["unaligned address aligns down", `st32(0x42, 9); return ld32(0x40);`],
    ["memmove overlapping", `st32(0x10, 0x01020304); memmove(0x10, 0x12, 0x1a); return ld32(0x14);`],
    ["memcmp equal", `st32(0x50, 5); st32(0x60, 5); return memcmp(0x50, 0x54, 0x60);`],
    ["memcmp differs", `st32(0x50, 0x04030201); st32(0x70, 0x04ff0201); return memcmp(0x50, 0x54, 0x70);`],
    ["slicecmp lengths", `st32(0x80, 1); st32(0x90, 1); return slicecmp(0x80, 0x84, 0x90, 0x93);`],

    // Traps.
    ["trap code", `trap(7);`],
    ["trap wraps negative", `trap(-1);`],
    ["conditional trap", `u32 x = 0; if (x == 0) { trap(3); } return 1;`],
]

let failures = 0
let compared = 0

for(const [label, source] of CASES)
{
    const gen: GenProgram = {procs: [{args: [], body: [...ir`${source}`.body]}]}

    let rtl
    try { rtl = lowerGen(gen, EXT); validateProgram(rtl, EXT) }
    catch(e) { console.log(`SETUP    ${label.padEnd(34)} ${(e as Error).message.split("\n")[0]}`); failures++; continue }

    EXT.reset()
    const a = evaluate(gen, [], EXT)
    const memA = Uint8Array.from(EXT.mem)

    EXT.reset()
    let vm
    try { vm = run(rtl, EXT, [], 200_000) }
    catch(e)
    {
        if(!(e instanceof UnspecifiedShiftAmount)) throw e
        compared++
        // Both sides refusing to answer is agreement — isa-core.md §4.1
        // defines no shift by 32 or more.
        if(a.outcome.kind !== "unspecified")
        { console.log(`MISMATCH ${label.padEnd(34)} ast=${a.outcome.kind} vm=unspecified   ${JSON.stringify(source)}`); failures++ }
        continue
    }
    const memB = Uint8Array.from(EXT.mem)

    compared++
    let why: string | null = null
    if(a.outcome.kind === "unspecified")
    { console.log(`MISMATCH ${label.padEnd(34)} ast=unspecified vm=ran   ${JSON.stringify(source)}`); failures++; continue }

    if(a.outcome.kind === "return" && vm.ok)
    {
        if((a.outcome.value >>> 0) !== (vm.acc >>> 0)) why = `ast=${a.outcome.value >>> 0} vm=${vm.acc >>> 0}`
    }
    else if(a.outcome.kind === "trap" && !vm.ok)
    {
        if((a.outcome.code >>> 0) !== ((vm.trapCode ?? 0) >>> 0)) why = `trap ast=${a.outcome.code >>> 0} vm=${vm.trapCode}`
    }
    else why = `outcome ast=${a.outcome.kind} vm=${vm.ok ? "return" : "trap"}`

    if(why === null)
    {
        for(let k = 0; k < memA.length; k++)
            if(memA[k] !== memB[k]) { why = `memory 0x${k.toString(16)} ast=${memA[k]} vm=${memB[k]}`; break }
    }

    if(why !== null) { console.log(`MISMATCH ${label.padEnd(34)} ${why}   ${JSON.stringify(source)}`); failures++ }
}

// A body with no `return` of its own establishes nothing, so what comes back
// is whatever `acc` holds — the last argument, which isa-core.md §4.6 puts
// there on entry. The generator closes every body with a valued return, so
// nothing it produces reaches this; the minimiser does, by deleting
// statements, and that is how the evaluator's own 0-instead-of-the-argument
// was found.
// Only the entry procedure can reach this: a body with no `return` is void,
// and the DSL refuses to use a void call as a value at all. `Executor::run`
// calls the entry directly, so an entry that falls off its end still hands
// back whatever acc holds.
const FALLS_THROUGH: [string, string[], string][] = [
    ["entry falls off its end, one arg", ["a"], ``],
    ["entry falls off its end, three args", ["a", "b", "c"], ``],
    ["entry falls off a branch", ["a"], `if (a > 1000000) { a = 1; }`],
    ["entry falls off after a store", ["a", "b"], `st32(16, a);`],
]

for(const [label, args, source] of FALLS_THROUGH)
{
    const gen: GenProgram = {procs: [{args, body: [...ir`${source}`.body]}]}

    let rtl
    try { rtl = lowerGen(gen, EXT); validateProgram(rtl, EXT) }
    catch(e) { console.log(`SETUP    ${label.padEnd(34)} ${(e as Error).message.split("\n")[0]}`); failures++; continue }

    const argv = entryArgsFor(rtl.procedures[0]!.argCount)
    EXT.reset()
    const a = evaluate(gen, argv, EXT)
    EXT.reset()
    const vm = run(rtl, EXT, argv, 200_000)
    compared++

    // Whatever the VM happens to report, the answer is a property of the
    // emitted code rather than of the program, so the evaluator refuses to
    // invent one and the driver drops the program instead of comparing it.
    if(a.outcome.kind !== "unspecified")
    {
        console.log(`MISMATCH ${label.padEnd(34)} expected unspecified, ast=${JSON.stringify(a.outcome)}`
            + ` (vm ${vm.ok ? vm.acc >>> 0 : "trap"}, accLive ${vm.accLive})`)
        failures++
    }
}

console.log(failures === 0 ? `eval: ${compared} cases, all agree` : `eval: ${failures} wrong`)
process.exit(failures === 0 ? 0 : 1)
