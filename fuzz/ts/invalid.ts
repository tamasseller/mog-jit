// fuzz — the invalid lane's runner.
//
//     npx ts-node --transpile-only fuzz/ts/invalid.ts [--rounds N] [--seed N]
//
// Takes programs the valid lane has already accepted, breaks exactly one
// named invariant in each, and requires the host toolchain to refuse it.
// Nothing downstream is asked: design.md §12 makes wire validity the
// validator's guarantee, so a program that gets past `validateProgram` is
// one the JIT will translate on trust. An escape here is therefore a hole
// in the only gate there is.
//
// Exits non-zero on an escape, and names the invariant that got through.

import { lowerProgram, run, validateProgram, StepLimitExceeded, UnspecifiedShiftAmount } from "mog-core"
import { rawMemExtension } from "./lib/rawmem_ext"
import { entryArgsFor } from "./lib/entry_args"
import { lowerGen, seedCorpus, toProceduresUnchecked } from "./gen/corpus"
import type { GenProgram } from "./gen/corpus"
import { mutateSequenced } from "./gen/mutate"
import { print } from "./gen/print"
import { BREAKERS } from "./gen/invalid"
import { Rng } from "./gen/rng"

const EXT = rawMemExtension()

const argv = process.argv.slice(2)
const value = (name: string, fallback: number): number =>
{
    const at = argv.indexOf(name)
    return at < 0 ? fallback : Number(argv[at + 1])
}
const ROUNDS = value("--rounds", 400)
const SEED0 = value("--seed", 1)

/** Which stage refused it, or null if none did. `roundTrip` is on, so the
 *  real parser gets the first vote — that is the path an application takes
 *  through `ir`. The graph is built unchecked, so what answers is mog-core
 *  and never this fuzzer's own `checkGraph`. */
function refusedBy(gen: GenProgram): string | null
{
    try
    {
        const rtl = lowerProgram(toProceduresUnchecked(gen, true), EXT)
        try { validateProgram(rtl, EXT) }
        catch { return "validate" }

        // Last resort: a violation the static gates cannot see by
        // construction, because it is a runtime value. §4.1's shift amount is
        // the only one, and the reference VM refusing to answer is the
        // language refusing — not a gate being asleep.
        try
        {
            EXT.reset()
            run(rtl, EXT, entryArgsFor(rtl.procedures[0]!.argCount), 50_000)
        }
        catch(e)
        {
            if(e instanceof UnspecifiedShiftAmount) return "vm"
            if(e instanceof StepLimitExceeded) return null
            return "vm"
        }
        return null
    }
    catch(e)
    {
        const m = (e as Error).message
        if((e as Error).name === "RoundTripError" || /SyntaxError|Expected /.test(m)) return "parse"
        return "lower"
    }
}

const caught: Record<string, Record<string, number>> = {}
const noSite: Record<string, number> = {}
const escaped: Record<string, number> = {}
const escapes: string[] = []

const corpus = seedCorpus()
for(let i = 0; i < ROUNDS; i++)
{
    const entry = corpus[i % corpus.length]!
    const base = mutateSequenced(entry.program, SEED0 + i)
    if(base === undefined) continue

    // Only break programs the valid lane would have accepted: otherwise a
    // refusal proves nothing about the breaker.
    if(refusedBy(base) !== null) continue

    const rng = new Rng(SEED0 + i)
    for(const breaker of BREAKERS)
    {
        const broken = breaker.apply(base, rng)
        if(broken === null) { noSite[breaker.name] = (noSite[breaker.name] ?? 0) + 1; continue }

        const stage = refusedBy(broken)
        if(stage === null)
        {
            escaped[breaker.name] = (escaped[breaker.name] ?? 0) + 1
            escapes.push(`${breaker.name}  (from ${entry.name}, seed ${SEED0 + i})\n`
                + broken.procs.map((p, k) => `--- p${k}(${p.args.join(", ")}) ---\n${print(p.body)}`).join(""))
            continue
        }
        caught[breaker.name] ??= {}
        caught[breaker.name]![stage] = (caught[breaker.name]![stage] ?? 0) + 1
    }
}

console.log(`invalid lane — ${ROUNDS} bases\n`)
for(const b of BREAKERS)
{
    const by = caught[b.name] ?? {}
    const total = Object.values(by).reduce((n, v) => n + v, 0)
    const where = Object.entries(by).sort((x, y) => y[1] - x[1]).map(([k, v]) => `${k} ${v}`).join(", ")
    const skipped = noSite[b.name] ?? 0
    console.log(`  ${String(total).padStart(5)} caught  ${b.name.padEnd(38)} ${where || "—"}`
        + (skipped > 0 ? `   (${skipped} with no site)` : ""))
}

if(escapes.length > 0)
{
    console.log(`\n${escapes.length} ESCAPE(S) — nothing refused these:`)
    for(const [k, v] of Object.entries(escaped).sort((a, b) => b[1] - a[1]))
        console.log(`  ${String(v).padStart(5)}  ${k}`)
    console.log()
    for(const e of escapes.slice(0, 3)) console.log(e)
}
else console.log("\nno escapes")

process.exit(escapes.length === 0 ? 0 : 1)
