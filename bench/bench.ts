// The benchmark driver: runs each image under QEMU with the counting
// plugin, differences the two sample counts into per-sample figures, reads
// the sizes out of the ELFs, and prints the table.
//
// Usage: npx ts-node --transpile-only bench/bench.ts [out-dir]

import {execFileSync, spawnSync} from "node:child_process"
import {readFileSync, existsSync} from "node:fs"
import {BENCH_N0, BENCH_N1, BENCH_N2} from "./bench-config"

const LEVELS = ["O0", "O1", "O2", "O3", "Os", "Og"] as const
type Level = (typeof LEVELS)[number]

const REGIONS = ["calibration", "mog_n0", "mog_n1", "mog_n2",
    "ref_n0", "ref_n1", "ref_n2"] as const
type Region = (typeof REGIONS)[number]

const PLUGIN = `${__dirname}/plugin/bench_plugin.so`

interface Expected
{
    workload: string
    result: number
    eventHash: number
    eventCount: number
    bytecodeBytes: number
    totalDepth: number
    maxCallDepth: number
    vmSteps: number
}

interface Counts
{
    insns: number
    cycles: number
}

interface Run
{
    regions: Record<Region, Counts>
    tags: Record<string, number>
}

function fail(msg: string): never
{
    console.error(`FAIL: ${msg}`)
    process.exit(1)
}

function symbolAddress(elf: string, name: string): string
{
    const out = execFileSync("arm-none-eabi-nm", [elf], {encoding: "utf8"})

    for(const line of out.split("\n"))
    {
        const parts = line.trim().split(/\s+/)
        if(parts.length === 3 && parts[2] === name) return parts[0]!
    }

    fail(`${elf}: no symbol ${name} — the marker was optimized away`)
}

/** `nm --print-size` in bytes, for one symbol. Matched on the demangled
 *  name: the kernels are C++, so `refPulseTrigger` is really
 *  `_Z15refPulseTriggerj` in the symbol table. */
function symbolSize(elf: string, name: string): number
{
    const out = execFileSync("arm-none-eabi-nm", ["--print-size", "--demangle", elf],
        {encoding: "utf8"})

    for(const line of out.split("\n"))
    {
        const parts = line.trim().split(/\s+/)
        if(parts.length >= 4 && parts.slice(3).join(" ").startsWith(`${name}(`))
        {
            return parseInt(parts[1]!, 16)
        }
    }

    fail(`${elf}: no sized symbol ${name}`)
}

/** Total .text of every symbol whose object contributed the JIT itself —
 *  the fixed cost of being able to run received bytecode at all. */
function fixedFootprintBytes(elf: string): number
{
    const out = execFileSync("arm-none-eabi-nm", ["--print-size", "--defined-only", elf],
        {encoding: "utf8"})

    /* Section-accurate attribution would need a link map; these are the
     * translator and runtime entry points that actually carry the weight,
     * and the figure is reported as what it is — an approximation of
     * design.md §15's estimate, not a substitute for a map file. */
    const prefixes = ["translate", "Runtime", "CodeArena", "DispatchTable", "Assembler",
        "Window", "AccState", "decodeInstr", "scanBody", "enterDispatch", "Executor",
        "_ZN4jitc", "_ZN8Assembler", "extEmit", "extDecode"]

    let total = 0

    for(const line of out.split("\n"))
    {
        const parts = line.trim().split(/\s+/)
        if(parts.length !== 4) continue

        const [, sizeHex, type, name] = parts
        if(type !== "T" && type !== "t") continue
        if(!prefixes.some(p => name!.startsWith(p) || name!.includes(p))) continue

        total += parseInt(sizeHex!, 16)
    }

    return total
}

function runImage(elf: string): Run
{
    const log = `${elf}.plugin.log`
    const args = REGIONS.map(r =>
        `region=${r}:${symbolAddress(elf, `bench_enter_${r}`)}:${symbolAddress(elf, `bench_exit_${r}`)}`)

    /* spawnSync, not execFileSync: under `target=native` the semihosting
     * writes land on stderr, which execFileSync discards on a clean exit —
     * and a clean exit is the normal case here. Both streams are kept, the
     * same way fuzz/qemu_exec/qemu_exec.ts does. */
    const proc = spawnSync("qemu-system-arm", [
        "-M", "microbit", "-nographic", "-monitor", "none", "-serial", "none",
        "-semihosting-config", "enable=on,target=native",
        "-plugin", [PLUGIN, ...args, "cycles=on", `out=${log}`].join(","),
        "-kernel", elf,
    ], {encoding: "utf8", timeout: 120_000})

    if(proc.error !== undefined) fail(`${elf}: qemu failed to start: ${proc.error.message}`)

    const output = (proc.stdout ?? "") + (proc.stderr ?? "")

    if(!existsSync(log)) fail(`${elf}: the plugin wrote no log — did it load?`)

    const regions = {} as Record<Region, Counts>

    for(const line of readFileSync(log, "utf8").split("\n"))
    {
        const m = /^REGION (\S+) insns=(\d+) cycles=(\d+) entries=(\d+)(.*)$/.exec(line.trim())
        if(m === null) continue

        if(m[5]!.includes("UNCLOSED")) fail(`${elf}: region ${m[1]} never closed`)
        if(m[4] !== "1") fail(`${elf}: region ${m[1]} ran ${m[4]} times, expected once`)

        regions[m[1] as Region] = {insns: Number(m[2]), cycles: Number(m[3])}
    }

    for(const r of REGIONS)
    {
        if(regions[r] === undefined) fail(`${elf}: region ${r} never reported`)
    }

    const tags: Record<string, number> = {}

    for(const line of output.split("\n"))
    {
        const m = /^([A-Z_]+):([0-9a-f]{8})$/.exec(line.trim())
        if(m !== null) tags[m[1]!] = parseInt(m[2]!, 16) >>> 0
    }

    return {regions, tags}
}

/** Stack usage of one function, from GCC's own -fstack-usage output. */
function stackUsage(suPath: string, fn: string): number | undefined
{
    if(!existsSync(suPath)) return undefined

    for(const line of readFileSync(suPath, "utf8").split("\n"))
    {
        const parts = line.split("\t")
        if(parts.length >= 2 && parts[0]!.includes(fn)) return Number(parts[1])
    }

    return undefined
}

function main(): void
{
    const outDir = process.argv[2] ?? `${process.env.TMPDIR ?? "/tmp"}/ppl-bench`
    const expected: Expected = JSON.parse(readFileSync(`${__dirname}/generated/bench_expected.json`, "utf8"))

    const samples = BENCH_N2 - BENCH_N1
    const rows: {level: Level; mog: number; ref: number; mogCy: number; refCy: number;
        translate: number; refStack: number | undefined; run: Run}[] = []

    for(const level of LEVELS)
    {
        const elf = `${outDir}/bench.${level}.elf`
        if(!existsSync(elf)) fail(`${elf} is missing — run bench/build.sh first`)

        const run = runImage(elf)
        const r = run.regions

        /* Correctness before throughput, on every image: a number from two
         * sides that disagree about the answer is not a measurement. */
        if(run.tags.DONE === undefined) fail(`${elf}: the image never reached DONE`)

        if(run.tags.MOG_RESULT !== expected.result)
        {
            fail(`${level}: the JIT returned ${run.tags.MOG_RESULT}, `
                + `the reference VM ${expected.result}`)
        }

        if(run.tags.REF_RESULT !== expected.result)
        {
            fail(`${level}: the compiled kernel returned ${run.tags.REF_RESULT}, `
                + `the reference VM ${expected.result}`)
        }

        if(run.tags.MOG_HASH !== expected.eventHash)
        {
            fail(`${level}: the JIT's trigger events hash to `
                + `0x${run.tags.MOG_HASH?.toString(16)}, the reference VM's to `
                + `0x${expected.eventHash.toString(16)}`)
        }

        if(run.tags.REF_HASH !== expected.eventHash)
        {
            fail(`${level}: the compiled kernel's trigger events hash to `
                + `0x${run.tags.REF_HASH?.toString(16)}, the reference VM's to `
                + `0x${expected.eventHash.toString(16)}`)
        }

        rows.push({
            level,
            mog: (r.mog_n2.insns - r.mog_n1.insns) / samples,
            ref: (r.ref_n2.insns - r.ref_n1.insns) / samples,
            mogCy: (r.mog_n2.cycles - r.mog_n1.cycles) / samples,
            refCy: (r.ref_n2.cycles - r.ref_n1.cycles) / samples,
            translate: r.mog_n0.insns - r.calibration.insns,
            refStack: stackUsage(`${outDir}/kernels_ref.${level}.su`, "refPulseTrigger"),
            run,
        })
    }

    /* Only kernels_ref.cpp's level moves, so every JIT-side figure must be
     * identical across the six images. It is a free consistency check on
     * the whole measurement, and the one thing that would catch the plugin
     * or the markers behaving differently from run to run. */
    const first = rows[0]!
    for(const row of rows.slice(1))
    {
        if(row.mog !== first.mog || row.mogCy !== first.mogCy
            || row.translate !== first.translate)
        {
            fail(`the JIT-side numbers moved between images (${first.level}: `
                + `${first.mog}/sample, ${row.level}: ${row.mog}/sample) — nothing that `
                + `should affect them changed, so the measurement is not reproducible`)
        }
    }

    const elf0 = `${outDir}/bench.${first.level}.elf`
    const mogCodeBytes = first.run.tags.MOG_CODE_BYTES!
    const fixed = fixedFootprintBytes(elf0)

    console.log(`\n# ${expected.workload}\n`)
    console.log(`${BENCH_N1} vs ${BENCH_N2} samples, differenced; `
        + `${expected.eventCount} triggers; every configuration agrees with the reference VM.\n`)

    console.log(`| level | cycles/sample | vs JIT | insns/sample | kernel .text | stack (GCC) |`)
    console.log(`|---|---|---|---|---|---|`)
    console.log(`| **JIT** | **${first.mogCy.toFixed(2)}** | 1.00x | `
        + `${first.mog.toFixed(2)} | `
        + `${mogCodeBytes} emitted + ${expected.bytecodeBytes} bytecode | — |`)

    for(const row of rows)
    {
        const size = symbolSize(`${outDir}/bench.${row.level}.elf`, "refPulseTrigger")
        console.log(`| ${row.level} | ${row.refCy.toFixed(2)} | `
            + `${(first.mogCy / row.refCy).toFixed(2)}x | ${row.ref.toFixed(2)} | ${size} | `
            + `${row.refStack ?? "?"} |`)
    }

    const t = first.run.tags
    const best = Math.min(...rows.map(r => r.refCy))
    const os = rows.find(r => r.level === "Os")!.refCy

    console.log(``)
    console.log(`## Cost of the JIT, once`)
    console.log(``)
    console.log(`- cold start: ${first.translate} instructions to translate this procedure `
        + `and set up the excursion, paid once`)
    console.log(`- fixed footprint: ~${fixed} bytes of .text (translator + runtime). `
        + `Approximate — attribution is by symbol name, not a link map.`)
    console.log(``)
    console.log(`## Stack`)
    console.log(``)

    /* The two JIT figures being equal is the result, not a copy-paste: the
     * N=0 phase does translation and no sample work, the N2 phase does
     * both, and they reach the same depth. */
    if(t.MOG_STACK === t.MOG_TRANSLATE_STACK)
    {
        console.log(`- JIT excursion peaks at ${t.MOG_STACK} bytes, and a translate-only run `
            + `reaches exactly the same depth — so the peak is set by the translator, not by `
            + `running the program. The program's own operand stack is `
            + `${expected.totalDepth} words (validator totalDepth).`)
    }
    else
    {
        console.log(`- JIT excursion peaks at ${t.MOG_STACK} bytes, of which a translate-only `
            + `run accounts for ${t.MOG_TRANSLATE_STACK}. Program operand stack: `
            + `${expected.totalDepth} words.`)
    }

    console.log(`- compiled kernel peaks at ${t.REF_STACK} bytes measured, against GCC's own `
        + `-fstack-usage figure for the kernel function alone (table above).`)
    console.log(``)
    console.log(`## Against docs/design.md`)
    console.log(``)
    console.log(`- §14 predicts throughput "within a small constant factor (roughly 2-4x) of `
        + `equivalent -Os C". Measured here: **${(first.mogCy / os).toFixed(2)}x** against `
        + `-Os, ${(first.mogCy / best).toFixed(2)}x against the best level — in modelled `
        + `Cortex-M0 cycles, which is what that claim is about.`)
    console.log(`- §14 predicts 1-3x opcode expansion for arithmetic-heavy code, 4-6x amortized `
        + `with control flow. This workload is almost all control flow and expands `
        + `${(mogCodeBytes / expected.bytecodeBytes).toFixed(2)}x by bytes `
        + `(${mogCodeBytes} emitted from ${expected.bytecodeBytes} bytecode).`)
    console.log(`- §15 estimates the whole JIT at 4-10 KB flash. Measured: ~${fixed} bytes.`)
    console.log(``)
    console.log(`One workload, one core. Cycles are modelled from ARM DDI 0432C's timings `
        + `over the exact executed instruction stream, not measured on silicon; `
        + `instruction counts beside them are exact. See bench/README.md before quoting `
        + `any of this.`)
    console.log(``)
}

main()
