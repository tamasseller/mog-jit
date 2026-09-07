// fuzz — what one program reaches that a campaign did not.
//
//     npx ts-node --transpile-only fuzz/ts/reach.ts prog.dsl [more.dsl ...]
//
// The campaign names the blocks it never ran; this closes the loop from the
// other end. Write a program aimed at one of them, run it here, and the
// answer is the blocks it lit up that /tmp/ppl-fuzz-coverage.bin has dark —
// which is the difference between believing a seed reaches something and
// knowing it does.
//
// Each file is DSL, in the `--- p<i>(args) ---` form findings are printed in.
import * as fs from "fs"
import * as path from "path"
import { encodeJitProgram, validateProgram, ir } from "mog-core"

import { rawMemExtension } from "./lib/rawmem_ext"
import { entryArgsFor } from "./lib/entry_args"
import { writeBatch, runQemu, unionCoverage, parseResults, BATCH_ADDR_DBG } from "./lib/batch"
import { covMap, loadCoverage } from "./lib/cov_map"
import { lowerGen } from "./gen/corpus"
import type { GenProgram } from "./gen/corpus"

const EXT = rawMemExtension()
const ELF = path.join(__dirname, "..", "src", "qemu-exec", "exec_runner_cov.elf")
const BATCH = "/tmp/ppl-reach-batch.bin"
const BASELINE = "/tmp/ppl-fuzz-coverage.bin"

/** `--- p0(a, b) ---` splits the procedures; anything before the first one
 *  is a single unnamed procedure taking one argument. */
function parseProgram(text: string): GenProgram
{
    const parts = text.split(/^---\s*p\d+\(([^)]*)\)\s*---$/m)
    if(parts.length === 1) return {procs: [{args: ["a"], body: [...ir`${text}`.body]}]}

    const procs: GenProgram["procs"] = []
    for(let i = 1; i < parts.length; i += 2)
    {
        const args = parts[i]!.split(",").map(a => a.trim()).filter(a => a !== "")
        procs.push({args, body: [...ir`${parts[i + 1]!}`.body]})
    }
    return {procs}
}

const map = covMap(ELF)
if(map === null) { console.log("could not read the coverage map from the ELF"); process.exit(1) }

const baseline = loadCoverage(BASELINE, ELF) ?? new Uint8Array(map.bitmapBytes)
const isSet = (bits: Uint8Array, b: number): boolean => ((bits[b >> 3] ?? 0) >> (b & 7) & 1) === 1

const where = new Map<number, {func: string; addr: number}>()
for(const s of map.sites) where.set(s.bucket, s)

for(const file of process.argv.slice(2))
{
    const program = parseProgram(fs.readFileSync(file, "utf8"))
    const rtl = lowerGen(program, EXT, false)
    validateProgram(rtl, EXT)
    writeBatch(BATCH, [{
        bytes: Buffer.from(encodeJitProgram(rtl, EXT)),
        entryArgs: entryArgsFor(rtl.procedures[0]!.argCount),
    }])

    const out = runQemu(ELF, BATCH, 60_000, BATCH_ADDR_DBG).output
    const bits = new Uint8Array(map.bitmapBytes)
    unionCoverage(out, bits)

    const fresh = map.sites.filter(s => isSet(bits, s.bucket) && !isSet(baseline, s.bucket))
    const result = parseResults(out)[0]

    console.log(`${path.basename(file)} — ${rtl.procedures.length} proc(s), `
        + `${result === undefined ? "no result" : `${result.kind}:${result.value.toString(16)}`}`)
    if(fresh.length === 0) { console.log("  nothing the campaign had not already reached\n"); continue }

    const addrs = fresh.map(s => s.addr.toString(16))
    const at = spawnLines(["arm-none-eabi-addr2line", "-e", ELF, ...addrs])
    const names = spawnLines(["arm-none-eabi-c++filt", ...fresh.map(s => s.func)])
    console.log(`  \x1b[32m${fresh.length} new block(s)\x1b[0m:`)
    fresh.forEach((s, i) => console.log(`    ${(names[i] ?? s.func).replace(/\(.*$/, "").padEnd(44)}`
        + ` ${(at[i] ?? "").replace(/^.*\//, "")}`))
    console.log()
}

function spawnLines(argv: string[]): string[]
{
    const r = require("child_process").spawnSync(argv[0], argv.slice(1), {encoding: "utf8"})
    return r.status === 0 ? r.stdout.split("\n").map((l: string) => l.trim()) : []
}
