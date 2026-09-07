// fuzz — what each seed is worth, measured the only way that matters:
// coverage of the DUT from one *unmutated* run of that seed alone.
//
//     npx ts-node --transpile-only fuzz/ts/seed_value.ts
//
// A seed's job is to start the campaign somewhere interesting. A seed that
// covers nothing another seed does not is not carrying its weight, whatever
// it once found — a fixed bug is pinned by a unit test, not by a corpus
// entry.
import * as fs from "fs"
import * as path from "path"
import { spawnSync } from "child_process"
import { encodeJitProgram, validateProgram } from "mog-core"
import { rawMemExtension } from "./lib/rawmem_ext"
import { entryArgsFor } from "./lib/entry_args"
import { writeBatch, runQemu, unionCoverage, BATCH_ADDR_DBG } from "./lib/batch"
import { covBitmapBytes } from "./lib/cov_map"
import { lowerGen, seedCorpus } from "./gen/corpus"
import type { GenProgram } from "./gen/corpus"

const HERE = __dirname
const COV_DRIVER = path.join(HERE, "..", "src", "driver", "fuzz_driver_cov")
const OBJ_DIR = path.join(HERE, "..", "src", "driver", ".o", "cov")
const BATCH = "/tmp/ppl-seedvalue-batch.bin"
const EXT = rawMemExtension()

/** .gcda files nest under the object dir mirroring absolute source paths, so
 *  this has to recurse — a flat readdir silently leaves them and every run
 *  then reports the accumulated total instead of its own. */
function wipeCounters(dir: string = OBJ_DIR): void
{
    for(const e of fs.readdirSync(dir, {withFileTypes: true}))
    {
        const full = path.join(dir, e.name)
        if(e.isDirectory()) wipeCounters(full)
        else if(e.name.endsWith(".gcda")) fs.unlinkSync(full)
    }
}

/** Every line gcovr reports as executed, as "file:line". */
function coveredLines(): Set<string>
{
    const r = spawnSync("gcovr", ["-r", path.join(HERE, "..", "..", "src"),
        "--object-directory", OBJ_DIR, "--json-summary-pretty", "--json-summary", "-"],
        {encoding: "utf8", maxBuffer: 256 * 1024 * 1024})
    if(r.status !== 0) throw new Error(`gcovr failed: ${r.stderr}`)

    // --json-summary gives per-file counts; the per-line detail needs the
    // full json report.
    const full = spawnSync("gcovr", ["-r", path.join(HERE, "..", "..", "src"),
        "--object-directory", OBJ_DIR, "--json", "-"],
        {encoding: "utf8", maxBuffer: 512 * 1024 * 1024})
    if(full.status !== 0) throw new Error(`gcovr --json failed: ${full.stderr}`)

    // Lines *and* branch outcomes. A seed that adds no new line can still be
    // the only one taking some branch the other way, and dropping it on a
    // line-only count would quietly lose that.
    const out = new Set<string>()
    for(const f of JSON.parse(full.stdout).files ?? [])
        for(const l of f.lines ?? [])
        {
            if(l.count > 0) out.add(`${f.file}:${l.line_number}`)
            for(const b of l.branches ?? [])
                if(b.count > 0) out.add(`${f.file}:${l.line_number}:b${b.branch_id}`)
        }
    return out
}

function runTogether(programs: readonly GenProgram[]): Set<string>
{
    wipeCounters()
    const entries = programs.map(p =>
    {
        const rtl = lowerGen(p, EXT, false)
        validateProgram(rtl, EXT)
        return {bytes: Buffer.from(encodeJitProgram(rtl, EXT)), entryArgs: entryArgsFor(rtl.procedures[0]!.argCount)}
    })
    writeBatch(BATCH, entries)
    const r = spawnSync(COV_DRIVER, [BATCH], {encoding: "utf8", timeout: 300_000})
    if(r.status !== 0) throw new Error(`driver exited ${r.status}`)
    return coveredLines()
}

function runAlone(program: GenProgram): Set<string>
{
    wipeCounters()

    const rtl = lowerGen(program, EXT, false)
    validateProgram(rtl, EXT)
    const bytes = Buffer.from(encodeJitProgram(rtl, EXT))
    writeBatch(BATCH, [{bytes, entryArgs: entryArgsFor(rtl.procedures[0]!.argCount)}])

    const r = spawnSync(COV_DRIVER, [BATCH], {encoding: "utf8", timeout: 120_000})
    if(r.status !== 0) throw new Error(`driver exited ${r.status}`)
    return coveredLines()
}

/** Target edges each seed reaches alone and unmutated, and how many of them
 *  no other seed reaches. One QEMU boot per seed and no `gcovr`, which is
 *  what makes it affordable at the start of every campaign — the host axis
 *  is the second opinion, and `hostUniqueItems` measures it only for the
 *  seeds this one finds idle. The two genuinely disagree: `ext_memcmp` has
 *  no unique host line and four unique target edges. */
export function targetEdgesPerSeed(): {name: string; edges: Set<number>; unique: number}[]
{
    const elf = path.join(HERE, "..", "src", "qemu-exec", "exec_runner_cov.elf")
    const rows = seedCorpus().map(({name, program}) =>
    {
        const rtl = lowerGen(program, EXT, false)
        validateProgram(rtl, EXT)
        writeBatch(BATCH, [{
            bytes: Buffer.from(encodeJitProgram(rtl, EXT)),
            entryArgs: entryArgsFor(rtl.procedures[0]!.argCount),
        }])
        const bits = new Uint8Array(covBitmapBytes(elf) ?? 1024)
        unionCoverage(runQemu(elf, BATCH, 60_000, BATCH_ADDR_DBG).output, bits)

        const edges = new Set<number>()
        for(let i = 0; i < bits.length; i++)
            for(let b = 0; b < 8; b++) if((bits[i]! >> b) & 1) edges.add(i * 8 + b)
        return {name, edges}
    })

    return rows.map(({name, edges}) =>
    {
        let unique = 0
        for(const e of edges) if(!rows.some(o => o.name !== name && o.edges.has(e))) unique++
        return {name, edges, unique}
    })
}

export interface HostVerdict
{
    /** Items lost by dropping that one seed, the rest of the corpus staying. */
    alone: Map<string, number>
    /** Items lost by dropping every seed that scored zero alone, all at once.
     *  Above zero means they cover each other rather than being spare, and
     *  one of them has to stay. */
    lostTogether: number
}

/** Host line+branch items that only the named seeds reach, by leave-one-out:
 *  one run of the whole corpus, then one per name without it. Costs
 *  `1 + names.length` runs instead of one per seed, which is why a campaign
 *  can afford to ask it about the few seeds the target axis calls idle.
 *
 *  Leave-one-out is blind to a mutually-redundant pair — each hides the
 *  other, both score zero, and deleting both loses what the pair covered.
 *  Hence the one further run over all the zero-scoring seeds at once. */
export function hostUniqueItems(names: readonly string[], onProgress?: (name: string) => void): HostVerdict
{
    const corpus = seedCorpus()
    onProgress?.("all")
    const base = runTogether(corpus.map(c => c.program))

    const lostWithout = (dropped: readonly string[]): number =>
    {
        const without = runTogether(corpus.filter(c => !dropped.includes(c.name)).map(c => c.program))
        let lost = 0
        for(const l of base) if(!without.has(l)) lost++
        return lost
    }

    const alone = new Map<string, number>()
    for(const name of names)
    {
        onProgress?.(name)
        alone.set(name, lostWithout([name]))
    }

    const spare = names.filter(n => alone.get(n) === 0)
    if(spare.length < 2) return {alone, lostTogether: 0}

    onProgress?.(spare.join(" and "))
    return {alone, lostTogether: lostWithout(spare)}
}

if(require.main !== module) { /* imported for the exports above */ }
else {

const corpus = seedCorpus()

// `--together <names...>`: one batch, all of them, to check the per-seed
// sets really do union to what a combined run reaches.
const tog = process.argv.indexOf("--together")
if(tog >= 0)
{
    const names = process.argv.slice(tog + 1)
    const pick = names.length > 0 ? corpus.filter(c => names.includes(c.name)) : corpus
    const covered = runTogether(pick.map(p => p.program))
    console.log(`${pick.length} seed(s) in one batch: ${covered.size} lines`)
    process.exit(0)
}

const per = new Map<string, Set<string>>()
for(const {name, program} of corpus)
{
    per.set(name, runAlone(program))
    process.stderr.write(`\r  ${name} — ${per.get(name)!.size} lines            `)
}
process.stderr.write("\r" + " ".repeat(60) + "\r")

const union = new Set<string>()
for(const s of per.values()) for(const l of s) union.add(l)

console.log(`${corpus.length} seeds, ${union.size} line+branch items covered by the corpus unmutated\n`)
console.log("seed                    alone   unique   (unique = items no other seed covers)")
const rows = corpus.map(({name}) =>
{
    const mine = per.get(name)!
    let unique = 0
    for(const l of mine)
    {
        let othersHaveIt = false
        for(const [n, s] of per) if(n !== name && s.has(l)) { othersHaveIt = true; break }
        if(!othersHaveIt) unique++
    }
    return {name, alone: mine.size, unique}
})
rows.sort((a, b) => b.unique - a.unique || b.alone - a.alone)
for(const r of rows) console.log(`${r.name.padEnd(22)} ${String(r.alone).padStart(6)}  ${String(r.unique).padStart(6)}`)

// Greedy: the smallest subset that still reaches every line the whole corpus does.
const need = new Set(union)
const keep: string[] = []
while(need.size > 0)
{
    let best = "", bestGain = -1
    for(const {name} of corpus)
    {
        if(keep.includes(name)) continue
        let gain = 0
        for(const l of per.get(name)!) if(need.has(l)) gain++
        if(gain > bestGain) { bestGain = gain; best = name }
    }
    if(bestGain <= 0) break
    keep.push(best)
    for(const l of per.get(best)!) need.delete(l)
}
console.log(`\nminimal covering subset (${keep.length} of ${corpus.length}): ${keep.join(" ")}`)
console.log(`redundant for coverage (${corpus.length - keep.length}): ${corpus.map(c => c.name).filter(n => !keep.includes(n)).join(" ")}`)

}
