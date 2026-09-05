// fuzz — the inner loop: the AST evaluator against the reference VM.
//
//     npx ts-node --transpile-only fuzz/ts/eval/diff.ts [count]
//
// This is the axis nothing else covers. The reference VM and the JIT both
// consume the *same* RtlProgram from the *same* lowerer, so a lowerer that
// emits valid-but-wrong RTL makes those two agree on the wrong answer. Only
// an engine that never sees the lowerer's output can see that — and this
// one costs no emulator, so it runs on everything before QEMU sees any of
// it.
//
// Values, trap codes and the whole extension buffer are compared. Two
// outcomes are dropped rather than compared: a shift amount of 32 or more
// (unspecified, isa-core.md §4.1) and anything hitting the step limit.

import { validateProgram, run, StepLimitExceeded, UnspecifiedShiftAmount } from "mog-core"
import { rawMemExtension } from "../lib/rawmem_ext"
import { entryArgsFor } from "../lib/entry_args"
import { seedCorpus, lowerGen } from "../gen/corpus"
import { mutateSequenced } from "../gen/mutate"
import { evaluate } from "./ast-eval"
import { print } from "../gen/print"

const EXT = rawMemExtension()
const seeds = seedCorpus()
const N = Number(process.argv[2] ?? 4000)
const MAX = 200_000

const bump = (o: Record<string, number>, k: string) => { o[k] = (o[k] ?? 0) + 1 }
const buckets: Record<string, number> = {}
const shown = new Set<string>()
let compared = 0, mismatched = 0

for(let i = 0; i < N; i++)
{
    const entry = seeds[i % seeds.length]!
    const seed = 0x2000 + i
    const gen = mutateSequenced(entry.program, seed)
    if(gen === undefined) { bump(buckets, "unsequenced"); continue }

    let rtl
    try { rtl = lowerGen(gen, EXT); validateProgram(rtl, EXT) }
    catch { bump(buckets, "rejected"); continue }

    const args = entryArgsFor(rtl.procedures[0]!.argCount)

    EXT.reset()
    const a = evaluate(gen, args, EXT, MAX)
    const memA = Uint8Array.from(EXT.mem)

    EXT.reset()
    let b: {ok: boolean; acc: number; trapCode: number | null} | "steplimit" | "unspec" | "threw"
    try { b = run(rtl, EXT, args, MAX) }
    catch(e)
    {
        b = e instanceof StepLimitExceeded ? "steplimit" : e instanceof UnspecifiedShiftAmount ? "unspec" : "threw"
        if(b === "threw") { bump(buckets, `vm threw: ${(e as Error).message.split("\n")[0]!.slice(0, 50)}`); continue }
    }
    const memB = Uint8Array.from(EXT.mem)

    if(a.outcome.kind === "unspecified" || b === "unspec") { bump(buckets, "unspecified (dropped)"); continue }
    if(a.outcome.kind === "steplimit" || b === "steplimit") { bump(buckets, "step limit (dropped)"); continue }

    compared++
    const vm = b as {ok: boolean; acc: number; trapCode: number | null}

    let why: string | null = null
    if(a.outcome.kind === "return" && vm.ok)
    {
        if((a.outcome.value >>> 0) !== (vm.acc >>> 0)) why = `value: ast=${a.outcome.value >>> 0} vm=${vm.acc >>> 0}`
    }
    else if(a.outcome.kind === "trap" && !vm.ok)
    {
        if((a.outcome.code >>> 0) !== ((vm.trapCode ?? 0) >>> 0)) why = `trap code: ast=${a.outcome.code >>> 0} vm=${vm.trapCode}`
    }
    else why = `outcome: ast=${a.outcome.kind} vm=${vm.ok ? "return" : "trap"}`

    if(why === null)
    {
        for(let k = 0; k < memA.length; k++)
            if(memA[k] !== memB[k]) { why = `memory at 0x${k.toString(16)}: ast=${memA[k]} vm=${memB[k]}`; break }
    }

    if(why !== null)
    {
        mismatched++
        const key = why.split(":")[0]!
        bump(buckets, `MISMATCH ${key}`)
        if(!shown.has(key))
        {
            shown.add(key)
            console.log(`\n=== ${entry.name}/${seed} — ${why}\n${gen.procs.map((p, k) => `--- p${k}(${p.args.join(", ")}) ---\n${print(p.body)}`).join("")}`)
        }
    }
}

console.log(`\n${N} mutants — ${compared} compared, ${mismatched} mismatched`)
for(const [k, v] of Object.entries(buckets).sort((a, b) => b[1] - a[1])) console.log(`  ${String(v).padStart(5)}  ${k}`)
