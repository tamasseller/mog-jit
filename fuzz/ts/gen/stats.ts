// fuzz — how much of what the generator produces survives the toolchain.
//
//     npx ts-node --transpile-only fuzz/ts/gen/stats.ts [count]
//
// The number that matters is the share reaching the reference VM: the whole
// point of generating at AST level is that validator approval becomes the
// normal outcome, so anything materially below it means a generator
// invariant is wrong — and the bucket it lands in names which.

import { validateProgram, run, encodeJitProgram, StepLimitExceeded, UnspecifiedShiftAmount } from "mog-core"
import { rawMemExtension } from "../lib/rawmem_ext"
import { entryArgsFor } from "../lib/entry_args"
import { seedCorpus, lowerGen } from "./corpus"
import { mutateSequenced, nestDepth } from "./mutate"
import { RoundTripError } from "./round_trip"

const EXT = rawMemExtension()
const seeds = seedCorpus()
const N = Number(process.argv[2] ?? 4000)

const buckets: Record<string, number> = {}
const bump = (k: string) => { buckets[k] = (buckets[k] ?? 0) + 1 }
const examples: Record<string, string> = {}

let maxNest = 0, maxProcs = 0, maxBytes = 0, withCalls = 0

for(let i = 0; i < N; i++)
{
    const entry = seeds[i % seeds.length]!
    const seed = 0x1000 + i
    try
    {
        // Unsequenced operands with conflicting effects have no single right
        // answer, so they never reach an engine.
        const gen = mutateSequenced(entry.program, seed)
        if(gen === undefined) { bump("unsequenced (gave up)"); continue }

        const rtl = lowerGen(gen, EXT)
        validateProgram(rtl, EXT)
        const bytes = encodeJitProgram(rtl, EXT)

        maxNest = Math.max(maxNest, ...gen.procs.map(p => nestDepth(p.body)))
        maxProcs = Math.max(maxProcs, rtl.procedures.length)
        maxBytes = Math.max(maxBytes, bytes.length)
        if(rtl.procedures.length > 1) withCalls++

        EXT.reset()
        try
        {
            const r = run(rtl, EXT, entryArgsFor(rtl.procedures[0]!.argCount), 200_000)
            bump(r.ok ? "ran" : "trapped")
        }
        catch(e)
        {
            if(e instanceof StepLimitExceeded) bump("step limit")
            else if(e instanceof UnspecifiedShiftAmount) bump("shift >= 32 (dynamic UB)")
            else { bump("vm threw"); examples["vm threw"] ??= `${entry.name}/${seed}: ${(e as Error).message.split("\n")[0]}` }
        }
    }
    catch(e)
    {
        const msg = (e as Error).message.split("\n")[0] ?? "?"
        const key = e instanceof RoundTripError ? "ROUND TRIP" : `rejected: ${msg.slice(0, 70)}`
        bump(key)
        examples[key] ??= `${entry.name}/${seed}`
    }
}

const total = Object.values(buckets).reduce((a, b) => a + b, 0)
const good = (buckets["ran"] ?? 0) + (buckets["trapped"] ?? 0)
console.log(`${N} mutants — ${good} lowered+validated+ran (${(100 * good / total).toFixed(1)}%)\n`)
for(const [k, v] of Object.entries(buckets).sort((a, b) => b[1] - a[1]))
    console.log(`  ${String(v).padStart(6)}  ${k}${examples[k] ? `   [${examples[k]}]` : ""}`)
console.log(`\nreach: maxNest=${maxNest} maxProcs=${maxProcs} maxBytes=${maxBytes} multiProc=${withCalls}`)
