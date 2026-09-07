// fuzz — the one entry point.
//
//     ./fuzz/fuzz.sh [--for 2h] [--rounds N] [--stop-on-finding]
//
// Everything else in `fuzz/` is a narrow piece with one job. This runs them
// in the one order that makes a campaign's answer mean anything:
//
//   calibrate     the oracles against known answers, and against each other
//   seed coverage what the corpus reaches before a single mutation
//   campaign      the main loop, with the lanes interleaved
//   report        where programs died, and how much of the target was reached
//
// Started with no arguments it runs until killed, which is the shape a
// server run wants; SIGINT prints the same report a bounded run does.
import * as fs from "fs"
import * as path from "path"
import { spawnSync } from "child_process"

import { runCampaign } from "./driver"
import type { CampaignResult, Finding } from "./driver"
import { seedCorpus } from "./gen/corpus"
import { targetEdgesPerSeed, hostUniqueItems } from "./seed_value"
import type { HostVerdict } from "./seed_value"
import { covMap, darkFunctions, saveCoverage, covBitmapBytes } from "./lib/cov_map"

const HERE = __dirname
const FUZZ = path.join(HERE, "..")
const COV_OUT = "/tmp/ppl-fuzz-coverage.bin"

// ── options ─────────────────────────────────────────────────────────────

const argv = process.argv.slice(2)
const flag = (n: string): boolean => argv.includes(n)
const value = (n: string): string | undefined =>
{
    const at = argv.indexOf(n)
    return at < 0 ? undefined : argv[at + 1]
}

/** `90s`, `20m`, `2h` — or a bare number, read as seconds. */
function duration(text: string): number
{
    const m = /^(\d+)([smh]?)$/.exec(text)
    if(m === null) throw new Error(`--for ${text}: expected something like 30s, 20m or 2h`)
    const n = Number(m[1])
    return n * (m[2] === "h" ? 3600_000 : m[2] === "m" ? 60_000 : 1000)
}

const FOR = value("--for")
const ROUNDS = value("--rounds")
const STOP_ON_FINDING = flag("--stop-on-finding")
const CALIBRATE_ONLY = flag("--calibrate-only")
const SKIP_SEED_COVERAGE = flag("--no-seed-coverage")
// How many batches apart the lanes run. They cost a second or so each, so a
// campaign spends a percent or two of itself on them.
const LANE_EVERY = Number(value("--lane-every") ?? 8)
// The one knob a second run wants: same everything, different sequence.
// Two runs at the same seed explore the same programs in the same order.
const SEED = value("--seed") === undefined ? undefined : Number(value("--seed"))

// Unbounded rounds when only a duration is given: the deadline is what
// stops it, and a round cap would end the run early and quietly.
const ROUNDS_N = ROUNDS !== undefined ? Number(ROUNDS) : Number.MAX_SAFE_INTEGER
const DEADLINE = FOR !== undefined ? Date.now() + duration(FOR) : undefined

// ── phases ──────────────────────────────────────────────────────────────

// A carriage-return ticker is right in a terminal and wrong in a log file,
// which is where a server run's output goes. Redirected, it becomes one
// unreadable line; so redirected, it becomes ordinary lines instead.
const TTY = process.stderr.isTTY === true

const bar = (t: string): void => console.log(`\n\x1b[1m── ${t} ${"─".repeat(Math.max(0, 62 - t.length))}\x1b[0m`)

/** Runs one of the narrow pieces as its own process. They stay standalone
 *  scripts — a second of startup each, once per campaign, buys nothing back
 *  by being folded in here, and a crash in one cannot take the run with it. */
function piece(label: string, script: string, args: readonly string[] = []): {ok: boolean; summary: string}
{
    const r = spawnSync("npx", ["ts-node", "--transpile-only", path.join(HERE, script), ...args],
        {encoding: "utf8", cwd: FUZZ, timeout: 900_000, maxBuffer: 64 * 1024 * 1024})
    const out = `${r.stdout ?? ""}${r.stderr ?? ""}`.trimEnd()
    // The headline, not the last line: several of these end on a breakdown
    // whose final row ("1 step limit (dropped)") says nothing about whether
    // the piece passed. A line that is not an indented breakdown row is.
    const lines = out.split("\n").filter(l => l.trim() !== "")
    const summary = [...lines].reverse().find(l => !/^\s/.test(l)) ?? lines[lines.length - 1] ?? "(no output)"
    const ok = r.status === 0
    console.log(`  ${ok ? "\x1b[32mok\x1b[0m  " : "\x1b[31mFAIL\x1b[0m"} ${label.padEnd(34)} ${summary}`)
    if(!ok) console.log(out.split("\n").map(l => `      ${l}`).join("\n"))
    return {ok, summary}
}

function build(): void
{
    bar("build")
    for(const [label, dir, args] of [
        ["target image", "src/qemu-exec", []],
        ["target image, asserts live", "src/qemu-exec", ["DEBUG=1"]],
        ["target image, instrumented", "src/qemu-exec", ["COV=1"]],
        ["host sink, ASan/UBSan", "src/driver", []],
    ] as const)
    {
        const r = spawnSync("make", ["-s", "-C", dir, ...args], {encoding: "utf8", cwd: FUZZ, timeout: 900_000})
        if(r.status !== 0)
        {
            console.log(`  \x1b[31mFAIL\x1b[0m ${label}\n${r.stdout ?? ""}${r.stderr ?? ""}`)
            process.exit(2)
        }
        console.log(`  \x1b[32mok\x1b[0m   ${label}`)
    }
}

function calibrate(): boolean
{
    bar("calibration — the oracles, before they are trusted")
    const results = [
        piece("AST evaluator vs known answers", "eval/eval_check.ts"),
        piece("UB analysis, both directions", "gen/ub_check.ts"),
        piece("evaluator vs reference VM", "eval/diff.ts"),
        piece("generator survival", "gen/stats.ts"),
        piece("coverage map vs the image", "cov_check.ts"),
    ]
    return results.every(r => r.ok)
}

function seedCoverage(): Uint8Array
{
    bar("seed coverage — what the corpus reaches unmutated")
    const rows = targetEdgesPerSeed()
    const total = new Set<number>()
    for(const r of rows) for(const e of r.edges) total.add(e)

    const union = new Uint8Array(covBitmapBytes(path.join(FUZZ, "src", "qemu-exec", "exec_runner_cov.elf")) ?? 1024)
    for(const e of total) union[e >> 3] |= 1 << (e & 7)

    const idle = rows.filter(r => r.unique === 0)
    console.log(`  ${rows.length} seeds reach ${total.size} target edges between them`)
    if(idle.length === 0)
    {
        console.log("  every seed contributes an edge no other one does")
        return union
    }

    console.log(`  ${idle.length} seed(s) contribute no target edge another does not — asking the host axis`)

    const host = hostAxis(idle.map(r => r.name))
    if(host === null)
    {
        console.log(`  \x1b[33munjudged\x1b[0m, the host axis did not run: ${idle.map(r => r.name).join(" ")}`)
        return union
    }

    const spare: string[] = []
    for(const r of idle)
    {
        const n = host.alone.get(r.name) ?? 0
        if(n > 0) console.log(`  \x1b[32mkeep\x1b[0m ${r.name.padEnd(18)} ${n} host line/branch items no other seed reaches`)
        else spare.push(r.name)
    }

    if(spare.length === 0) return union
    if(host.lostTogether > 0)
        console.log(`  \x1b[33mkeep one of\x1b[0m ${spare.join(" ")} — each covers the others, `
            + `and ${host.lostTogether} host items go if all of them do`)
    else for(const name of spare)
        console.log(`  \x1b[33mdrop\x1b[0m ${name.padEnd(18)} nothing unique on either axis`)

    return union
}

/** The host axis costs a `gcovr` run per question, so it is asked only about
 *  the seeds the target axis already called idle, and only then is its sink
 *  built. Returns null rather than failing the campaign: a missing `gcovr`
 *  leaves a seed unjudged, which is not a reason to refuse to fuzz. */
function hostAxis(names: readonly string[]): HostVerdict | null
{
    const built = spawnSync("make", ["-s", "-C", "src/driver", "COV=1"],
        {encoding: "utf8", cwd: FUZZ, timeout: 900_000})
    if(built.status !== 0) return null

    try
    {
        return hostUniqueItems(names, n => { if(TTY) process.stderr.write(
            `\r  host coverage: ${n === "all" ? "the whole corpus" : `without ${n}`}${" ".repeat(24)}`) })
    }
    catch { return null }
    finally { if(TTY) process.stderr.write(`\r${" ".repeat(70)}\r`) }
}

function lanes(batches: number): boolean
{
    if(batches % LANE_EVERY !== 0) return true
    const a = piece("invalid lane", "invalid.ts", ["--rounds", "120", "--seed", String(batches * 977)])
    const b = piece("frame lane", "frame.ts", ["--rounds", "60", "--seed", String(batches * 641)])
    return a.ok && b.ok
}

/** The deliverable. A finding arrives as whatever mutation happened to
 *  produce it — tens of statements of noise around the two or three that
 *  matter — so it is shrunk before it is shown. What comes out is DSL
 *  source: readable, re-runnable, and already in the format a seed file
 *  takes. */
function minimizeAndShow(findings: readonly Finding[]): void
{
    bar(`findings — ${findings.length}, minimized`)
    for(const f of findings)
    {
        console.log(`\n\x1b[31m${f.kind}\x1b[0m  from ${f.entry}, seed ${f.seed}`)
        console.log(`  ${f.detail.split("\n")[0]}`)
        if(f.file === undefined) { console.log("  (no program attached — nothing to shrink)"); continue }

        const args = [f.file, ...(f.needsTarget ? ["--jit"] : [])]
        const r = spawnSync("npx", ["ts-node", "--transpile-only", path.join(HERE, "gen/minimize.ts"), ...args],
            {encoding: "utf8", cwd: FUZZ, timeout: 1800_000, maxBuffer: 64 * 1024 * 1024})
        const out = `${r.stdout ?? ""}`.trimEnd()

        if(r.status !== 0 || !out.includes("--- p0("))
        {
            // Worth saying out loud rather than swallowing: a finding that
            // will not shrink is still a finding, and the saved program is
            // still the repro.
            console.log(`  could not shrink it — the saved program is the repro: ${f.file}`)
            continue
        }
        console.log(out.split("\n").filter(l => !/^\d+ candidate/.test(l) && !l.startsWith("wrote ")).join("\n"))
    }
}

function report(r: CampaignResult, laneFailure: boolean): void
{
    bar("report")

    const take = (pred: (k: string) => boolean): [string, number][] =>
        Object.entries(r.counts).filter(([k]) => pred(k)).sort((a, b) => b[1] - a[1])
    const sum = (rows: [string, number][]): number => rows.reduce((n, [, v]) => n + v, 0)

    const dropped = take(k => k.startsWith("dropped:"))
    const bails = take(k => k.startsWith("resource bail"))
    const reached = r.counts["verified"] ?? 0
    const agreed = r.counts["target agrees"] ?? 0

    console.log(`  ran ${r.seconds.toFixed(1)}s over ${r.batches} batch(es)`)
    console.log(`  generated              ${String(sum(dropped) + reached).padStart(8)}`)
    console.log(`  distinct shapes        ${String(r.shapes).padStart(8)}`)
    console.log(`  corpus                 ${String(r.corpusSize).padStart(8)}`)

    console.log(`\n  dropped before the target ${String(sum(dropped)).padStart(5)}`)
    for(const [k, v] of dropped) console.log(`    ${String(v).padStart(8)}  ${k.replace(/^dropped: /, "")}`)

    console.log(`\n  reached the target     ${String(reached).padStart(8)}`)
    console.log(`    ${String(agreed).padStart(8)}  agreed everywhere — the whole pipeline, end to end`)
    if(bails.length > 0)
    {
        console.log(`    ${String(sum(bails)).padStart(8)}  refused by the target, legitimately (design.md §12)`)
        for(const [k, v] of bails) console.log(`      ${String(v).padStart(6)}  ${k.replace("resource bail ", "0x")}`)
    }
    for(const [k, v] of take(k => !k.startsWith("dropped:") && !k.startsWith("resource bail")
        && k !== "verified" && k !== "target agrees"))
        console.log(`    ${String(v).padStart(8)}  ${k}`)

    reportCoverage(r)

    if(r.findings.length === 0 && !laneFailure) console.log("\n\x1b[32m  no findings\x1b[0m")
    else console.log(`\n\x1b[31m  ${r.findings.length} FINDING(S)\x1b[0m${laneFailure ? " — and a lane failed, see above" : ""}`)
}

/** The bitmap holds 2048 bits and the image has nothing like that many
 *  blocks, so the bits alone say nothing about how much of the target ran.
 *  Against the buckets the image's own blocks hash into, they do. */
function reportCoverage(r: CampaignResult): void
{
    if(r.coverageBits === 0) return

    const map = covMap(path.join(FUZZ, "src", "qemu-exec", "exec_runner_cov.elf"))
    if(map === null)
    {
        console.log(`\n  target coverage        ${String(r.coverageBits).padStart(8)} bitmap bits`
            + ` (of ${r.coverageBitsTotal}; naming them needs arm-none-eabi-objdump)`)
        return
    }

    const blocks = map.sites.length
    const pct = 100 * r.coverageBits / blocks
    console.log(`\n  target coverage        ${String(r.coverageBits).padStart(8)} of ${blocks}`
        + ` basic blocks  (${pct.toFixed(1)}%)`)

    // Kept so the dark set can be picked over without fuzzing again — the
    // list below is the headline, `ts/dark.ts` is the whole of it.
    saveCoverage(COV_OUT, path.join(FUZZ, "src", "qemu-exec", "exec_runner_cov.elf"), r.coverage)

    const dark = darkFunctions(map, r.coverage)
    if(dark.length === 0) { console.log("  every instrumented block ran"); return }

    console.log(`\n  ${dark.reduce((n, d) => n + d.dark, 0)} block(s) never ran, worst first:`)
    for(const d of dark.slice(0, 12))
        console.log(`    ${String(d.dark).padStart(4)} of ${String(d.total).padEnd(4)} ${d.func.padEnd(42)} ${d.lines.join(" ")}`)
    if(dark.length > 12) console.log(`    ${" ".repeat(12)}and ${dark.length - 12} more function(s)`)
    console.log(`  all of it: npx ts-node --transpile-only fuzz/ts/dark.ts`)
}

// ── run ─────────────────────────────────────────────────────────────────

build()
if(!calibrate())
{
    console.log("\n\x1b[31mcalibration failed — a campaign now would be comparing something against itself\x1b[0m")
    process.exit(2)
}
if(CALIBRATE_ONLY)
{
    bar("lanes — once, at full volume")
    const ok = piece("invalid lane", "invalid.ts", ["--rounds", "500"]).ok
        && piece("frame lane", "frame.ts", ["--rounds", "300"]).ok
    console.log(ok ? "\n\x1b[32m  all self-checks passed\x1b[0m" : "\n\x1b[31m  a lane failed\x1b[0m")
    process.exit(ok ? 0 : 1)
}
const seedBits = SKIP_SEED_COVERAGE ? undefined : seedCoverage()

bar(`campaign — ${FOR !== undefined ? `for ${FOR}` : ROUNDS !== undefined ? `${ROUNDS} rounds` : "until killed"}`)
console.log(`  ${seedCorpus().length} seeds, lanes every ${LANE_EVERY} batches, findings under /tmp/ppl-fuzz-findings\n`)

let laneFailure = false
let interrupted = false
process.on("SIGINT", () => { interrupted = true })

const LOG_EVERY_MS = 30_000
let lastLogged = 0
let ticking = false
const clearTicker = (): void => { if(ticking) { process.stderr.write("\r" + " ".repeat(78) + "\r"); ticking = false } }

const result = runCampaign({
    coverage0: seedBits,
    rounds: ROUNDS_N,
    seed: SEED,
    deadline: DEADLINE,
    stopOnFinding: STOP_ON_FINDING,
    onProgress: (done, rounds, batches, shapes, corpusSize) =>
    {
        const of = rounds === Number.MAX_SAFE_INTEGER ? "" : `/${rounds}`
        const left = DEADLINE === undefined ? "" : `, ${Math.max(0, Math.round((DEADLINE - Date.now()) / 1000))}s left`
        const line = `${done}${of} — ${batches} batch(es), ${shapes} shapes, corpus ${corpusSize}${left}`
        if(TTY) { process.stderr.write(`\r  ${line}   `); ticking = true }
        // Redirected, on a clock rather than on a batch count: a log has to
        // show the run is alive over hours without filling the disk, and
        // tying it to any batch cadence makes it silent whenever that
        // cadence is long.
        else if(Date.now() - lastLogged >= LOG_EVERY_MS) { lastLogged = Date.now(); console.log(`  ${line}`) }
    },
    betweenBatches: (batches) =>
    {
        if(interrupted) { clearTicker(); return false }
        if(batches % LANE_EVERY === 0) clearTicker()
        if(!lanes(batches)) { laneFailure = true; return false }
        return true
    },
})
clearTicker()

if(interrupted) console.log("\n  interrupted")
report(result, laneFailure)
if(result.findings.length > 0) minimizeAndShow(result.findings)
process.exit(result.findings.length === 0 && !laneFailure ? 0 : 1)
