// fuzz — the campaign driver.
//
//     npx ts-node --transpile-only fuzz/ts/driver.ts [--rounds N] [--batch N] [--seed N]
//         [--no-qemu] [--no-host] [--no-dbg] [--qemu-elf F] [--host-driver F]
//
// TS generates and owns the campaign; the out-of-process sinks are handed
// batches of programs that already passed everything cheap.
//
// Two tiers, because they cost three orders of magnitude apart:
//
//   inner   mutate → UB filter → evaluate → lower → validate → run the
//           reference VM → compare. No emulator, thousands per second, and
//           the only tier that can see a *lowerer* bug: the reference VM and
//           the JIT both consume the same RtlProgram, so only an engine that
//           never sees the lowerer's output can disagree with it.
//
//   outer   whatever survived, in batches, to the emulated target (wrong
//           answers from real emitted Thumb), to the same target image built
//           with asserts live, and to the host translator under ASan/UBSan
//           (crashes). No sink sees another's findings: the host one never
//           executes what it emits, and the shipped target image is
//           -DNDEBUG and so cannot see an assert fire.
//
// A finding is written out as a program under `--save`; the `(corpus entry,
// seed)` it is reported under names the seed it descends from.

import * as fs from "fs"
import * as path from "path"
import { encodeJitProgram, run, validateProgram, checkProfile, ARMV6M_PROFILE, StepLimitExceeded, UnspecifiedShiftAmount } from "mog-core"
import type { RtlProgram } from "mog-core"
import { rawMemExtension } from "./lib/rawmem_ext"
import { entryArgsFor } from "./lib/entry_args"
import { chunk, digestOf, parseResults, runQemu, unionCoverage, writeBatch, BATCH_ADDR, BATCH_ADDR_DBG, PROGRAM_MAX } from "./lib/batch"
import { covBitmapBytes } from "./lib/cov_map"
import { lowerGen, seedCorpus } from "./gen/corpus"
import type { GenProgram } from "./gen/corpus"
import { inflateSequenced, mutateSequenced, nestDepth } from "./gen/mutate"
import { print } from "./gen/print"
import { saveProgram } from "./gen/program_file"
import { evaluate } from "./eval/ast-eval"

const EXT = rawMemExtension()
const HERE = __dirname
const EXEC_ELF_DEFAULT = path.join(HERE, "..", "src", "qemu-exec", "exec_runner.elf")
// The same runner with asserts live. The shipped image is -DNDEBUG, so
// nothing that executes emitted Thumb has ever been able to see an assert
// fire: the host sink asserts but never runs the code, this one runs it and
// was blind. That is the only reason this ELF exists — its answers are the
// other one's by construction.
const EXEC_ELF_DBG = path.join(HERE, "..", "src", "qemu-exec", "exec_runner_dbg.elf")
const HOST_DRIVER_DEFAULT = path.join(HERE, "..", "src", "driver", "fuzz_driver")
// Per process, so several workers can run a campaign at once without
// clobbering each other's batch between writing it and running it.
const BATCH_PATH = `/tmp/ppl-fuzz-driver-batch-${process.pid}.bin`

const MAX_STEPS = 200_000
const QEMU_TIMEOUT_MS = 20_000

// What the sinks can hold, which is not what the target can encode. The
// latter is `ARMV6M_PROFILE` (mog-core profile.ts), and `encodeJitProgram`
// enforces it; these two are this harness's own capacity — mirroring
// `fuzz/src/qemu-exec/exec_runner.cpp`'s `ENTRY_ARGS_MAX` and
// `fuzz/src/driver/harness.cpp`'s `MAX_PROC_COUNT`, both sized off 8KB of
// target RAM. A batch naming more is refused by the sink, not truncated.
const HARNESS_MAX_ARG_COUNT = 16
const HARNESS_MAX_PROC_COUNT = 16

// ── options ─────────────────────────────────────────────────────────────

const argv = process.argv.slice(2)
const flag = (name: string): boolean => argv.includes(name)
const value = (name: string, fallback: number): number =>
{
    const at = argv.indexOf(name)
    return at < 0 ? fallback : Number(argv[at + 1])
}

let ROUNDS = value("--rounds", 2000)
const BATCH = value("--batch", 200)
let SEED0 = value("--seed", 1)
// Where a finding's program is written. The pair a finding is reported
// under names the *seed* it descends from, not the derived program that
// actually failed — the corpus grows, so by the time something fails its
// entry is usually several generations deep. The program is the repro.
const at = argv.indexOf("--save")
const SAVE_DIR = at < 0 ? "/tmp/ppl-fuzz-findings" : argv[at + 1]!
// How often the size-directed lane runs instead of ordinary mutation, and
// how long a run it builds. Mutation adds a statement at a time against a
// corpus bounded well below a branch's reach, so the translator's
// out-of-range paths (RESOURCE_LIMIT_BRANCH_RANGE, and every `return false`
// that propagates one) are unreachable from it however long a campaign runs.
// The ladder straddles both thresholds: a conditional branch reaches ±254
// bytes of code and the wide form the translator retries in reaches ±2046.
// How many batches apart the assert image runs. Not every batch: it is a
// second QEMU boot over programs the first has already answered, and only
// its asserts are new.
const DBG_EVERY = value("--dbg-every", 4)

// Coverage is a third boot over programs both other images have already
// answered, so it runs on its own cadence rather than every batch. What it
// measures is where the campaign has *been*, which moves slowly.
const COV_EVERY = value("--cov-every", 4)

const BULK_EVERY = value("--bulk-every", 32)
const BULK_SIZES = [80, 160, 240, 320]

const USE_QEMU = !flag("--no-qemu")
const USE_HOST = !flag("--no-host")
const USE_DBG = !flag("--no-dbg")
const USE_COV = !flag("--no-cov")
// Which build of the host sink a campaign feeds. The default is the
// ASan/UBSan one that hunts crashes; `src/driver/fuzz_driver_cov` is the
// same code built for line coverage, and answers the other question — which
// translator branches a campaign never reaches at all.
const hostAt = argv.indexOf("--host-driver")
const HOST_DRIVER = hostAt < 0 ? HOST_DRIVER_DEFAULT : path.resolve(argv[hostAt + 1]!)
// Which target image answers. `exec_runner_cov.elf` is the same code with
// `-fsanitize-coverage=trace-pc`, and prints an edge bitmap per boot; its
// rom is larger, so its batch starts somewhere else.
const qemuAt = argv.indexOf("--qemu-elf")
const EXEC_ELF = qemuAt < 0 ? EXEC_ELF_DEFAULT : path.resolve(argv[qemuAt + 1]!)
const EXEC_ADDR = qemuAt < 0 ? BATCH_ADDR : BATCH_ADDR_DBG
const coverage = new Uint8Array(covBitmapBytes(path.join(HERE, "..", "src", "qemu-exec", "exec_runner_cov.elf")) ?? 1024)
let coverageBits = 0

// ── the inner loop ──────────────────────────────────────────────────────

interface Verified
{
    entry: string
    seed: number
    gen: GenProgram
    bytes: Buffer
    entryArgs: number[]
    /** What both reference engines agreed on. */
    expected: {kind: "return"; value: number} | {kind: "trap"; code: number}
    expectedDigest: number
    /** Cheap structural fingerprint, for retention. */
    signature: string
}

const counts: Record<string, number> = {}
const bump = (k: string): void => { counts[k] = (counts[k] ?? 0) + 1 }

export interface Finding
{
    kind: string
    entry: string
    seed: number
    detail: string
    /** Where the program was written, for a minimizer to pick up. */
    file?: string
    /** Whether shrinking it needs the target, or only the two host engines.
     *  A disagreement the emulator found cannot be reproduced without it. */
    needsTarget: boolean
    text: string
}

/** The kinds only the emitted code can show. Everything else is a
 *  disagreement between the evaluator and the reference VM, reproducible in
 *  process and shrunk a thousand times faster. */
const TARGET_KINDS = new Set(["HANG", "BATCH STOPPED", "JIT", "DBG HANG", "TARGET ASSERT", "CRASH"])

const findings: Finding[] = []

function report(kind: string, entry: string, seed: number, detail: string, gen?: GenProgram): void
{
    let saved = ""
    let file: string | undefined
    if(gen !== undefined)
    {
        fs.mkdirSync(SAVE_DIR, {recursive: true})
        file = path.join(SAVE_DIR, `${kind.toLowerCase().replace(/[^a-z]+/g, "-")}-${entry}-${seed}.json`)
        saveProgram(file, gen)
        saved = `\n  saved ${file}`
    }
    const source = gen === undefined ? ""
        : "\n" + gen.procs.map((p, i) => `--- p${i}(${p.args.join(", ")}) ---\n${print(p.body)}`).join("")
    const text = `${kind}  descended from ${entry}, round seed ${seed}${saved}\n  ${detail}${source}`
    findings.push({kind, entry, seed, detail, file, needsTarget: TARGET_KINDS.has(kind), text})
    console.log(`\n${text}`)
}

/** Everything that costs no emulator. Returns a program only when the AST
 *  evaluator and the reference VM agree on it. */
function verify(entry: string, gen: GenProgram | undefined, seed: number): Verified | null
{
    if(gen === undefined) { bump("dropped: ub filter"); return null }

    // `true`: every candidate goes through `parse(print(ast))` and is
    // lowered from the *reparsed* tree, so the program that runs is provably
    // the one printed in a report. This used to be a sampled out-of-band
    // sweep over the corpus, because parsing dominated the inner loop — a
    // cost that turned out to be two grammar rules parsing their operand
    // twice, not the parser generator. Fixed in mog-core, it now costs a few
    // percent of a campaign.
    //
    // Lowering and validation are caught apart, not together: which of the
    // two refused says which invariant drifted, and the final report is
    // organised by the stage a program died at.
    let rtl: RtlProgram
    try { rtl = lowerGen(gen, EXT, true) }
    catch(e)
    {
        // The generator is supposed to produce only well-formed programs, so
        // this is a generator bug rather than a finding — reported, because
        // a rejection rate that climbs means an invariant has drifted.
        bump(`dropped: lowerer — ${(e as Error).message.split("\n")[0]!.slice(0, 52)}`)
        return null
    }

    let stats
    try { stats = validateProgram(rtl, EXT) }
    catch(e)
    {
        bump(`dropped: validator — ${(e as Error).message.split("\n")[0]!.slice(0, 50)}`)
        return null
    }

    // `encodeJitProgram` below would refuse either of these too. Reported
    // here instead of caught there because the generator is supposed to stay
    // inside both on its own: one of these is drift, not a finding.
    const outOfProfile = checkProfile(rtl, stats, ARMV6M_PROFILE, EXT)
    if(outOfProfile.length > 0)
    {
        report("OUT OF PROFILE", entry, seed, outOfProfile[0]!.message, gen)
        return null
    }

    if(rtl.procedures.length > HARNESS_MAX_PROC_COUNT
        || rtl.procedures.some(p => p.argCount > HARNESS_MAX_ARG_COUNT))
    {
        report("OVER HARNESS CAPACITY", entry, seed,
            `${rtl.procedures.length} procedure(s), deepest argCount `
            + `${Math.max(...rtl.procedures.map(p => p.argCount))}`, gen)
        return null
    }

    let bytes: Buffer
    try { bytes = Buffer.from(encodeJitProgram(rtl, EXT)) }
    catch(e) { bump(`dropped: encoder — ${(e as Error).message.split("\n")[0]!.slice(0, 50)}`); return null }
    if(bytes.length === 0 || bytes.length > PROGRAM_MAX) { bump("dropped: harness — too large"); return null }

    const entryArgs = entryArgsFor(rtl.procedures[0]!.argCount)

    EXT.reset()
    const ast = evaluate(gen, entryArgs, EXT, MAX_STEPS)
    const astDigest = digestOf(EXT.mem)

    EXT.reset()
    let vm
    try { vm = run(rtl, EXT, entryArgs, MAX_STEPS) }
    catch(e)
    {
        if(e instanceof StepLimitExceeded) { bump("dropped: reference VM — step limit"); return null }
        if(e instanceof UnspecifiedShiftAmount)
        {
            // Both refusing to answer is agreement (isa-core.md §4.1).
            if(ast.outcome.kind !== "unspecified")
                report("LOWERER", entry, seed, `evaluator answered ${ast.outcome.kind}, reference VM says the shift is unspecified`, gen)
            bump("dropped: reference VM — unspecified")
            return null
        }
        report("VM THREW", entry, seed, (e as Error).message.split("\n")[0]!, gen)
        return null
    }
    const vmDigest = digestOf(EXT.mem)

    if(ast.outcome.kind === "unspecified") { bump("dropped: evaluator — unspecified"); return null }
    if(ast.outcome.kind === "steplimit") { bump("dropped: evaluator — step limit"); return null }

    // A void entry has no specified result (§8.7); the generator closes
    // every body with a valued return, so this should not arise.
    if(vm.ok && !vm.accLive) { bump("dropped: void entry"); return null }

    const expected = ast.outcome.kind === "return"
        ? {kind: "return" as const, value: ast.outcome.value >>> 0}
        : {kind: "trap" as const, code: ast.outcome.code >>> 0}

    let disagreement: string | null = null
    if(expected.kind === "return" && vm.ok)
    {
        if(expected.value !== (vm.acc >>> 0)) disagreement = `value: evaluator ${expected.value}, reference VM ${vm.acc >>> 0}`
    }
    else if(expected.kind === "trap" && !vm.ok)
    {
        if(expected.code !== ((vm.trapCode ?? 0) >>> 0)) disagreement = `trap code: evaluator ${expected.code}, reference VM ${vm.trapCode}`
    }
    else disagreement = `outcome: evaluator ${expected.kind}, reference VM ${vm.ok ? "return" : "trap"}`

    if(disagreement === null && astDigest !== vmDigest)
        disagreement = `extension buffer: evaluator ${astDigest.toString(16)}, reference VM ${vmDigest.toString(16)}`

    if(disagreement !== null)
    {
        // Neither engine sees the other's code, and only one of them is
        // downstream of the lowerer — so this is the lowerer or the VM,
        // never the JIT.
        report("LOWERER/VM", entry, seed, disagreement, gen)
        bump("A/B mismatch")
        return null
    }

    bump("verified")

    // What the validator already measured, plus the tree's own shape:
    // operand-stack depth and call depth are the axes the target sizes its
    // reservation from, so a new pair is genuinely a new program to run
    // rather than another mutant of one already covered.
    const depth = Math.max(...gen.procs.map(p => nestDepth(p.body)))
    const signature = [gen.procs.length, depth, stats.totalDepth, stats.maxCallDepth,
        bytes.length >> 4, rtl.procedures.reduce((n, p) => n + p.body.length, 0) >> 3].join("/")

    return {entry, seed, gen, bytes, entryArgs, expected, expectedDigest: vmDigest, signature}
}

// ── the outer loop ──────────────────────────────────────────────────────

function runTarget(batch: Verified[]): void
{
    writeBatch(BATCH_PATH, batch)
    const r = runQemu(EXEC_ELF, BATCH_PATH, QEMU_TIMEOUT_MS, EXEC_ADDR)
    coverageBits += unionCoverage(r.output, coverage)

    if(r.timedOut)
    {
        const done = parseResults(r.output).length
        const culprit = batch[done]
        bump("qemu hang")
        if(culprit !== undefined)
            report("HANG", culprit.entry, culprit.seed, `emitted code did not finish (${r.status})`, culprit.gen)
        return
    }

    const results = parseResults(r.output)
    if(results.length !== batch.length)
    {
        const stopped = batch[results.length]
        report("BATCH STOPPED", stopped?.entry ?? "?", stopped?.seed ?? 0,
            `runner reported ${results.length} of ${batch.length}`, stopped?.gen)
        return
    }

    batch.forEach((c, i) =>
    {
        const {kind, value, digest} = results[i]!
        if(kind === "X") { bump("target rejected"); return }
        if(kind === "E") { bump(`resource bail ${value.toString(16)}`); return }

        const agrees = c.expected.kind === "return"
            ? kind === "R" && value === c.expected.value
            : kind === "T" && value === c.expected.code

        const memAgrees = digest === null || digest === c.expectedDigest

        if(agrees && memAgrees) { bump("target agrees"); return }

        const shown = kind === "R" ? `RETURN ${value}` : `TRAP ${value}`
        const want = c.expected.kind === "return" ? `RETURN ${c.expected.value}` : `TRAP ${c.expected.code}`
        report("JIT", c.entry, c.seed,
            `reference ${want}, emitted Thumb ${shown}`
            + (memAgrees ? "" : `\n  extension buffer: reference ${c.expectedDigest.toString(16)}, target ${(digest ?? 0).toString(16)}`),
            c.gen)
    })
}

/** The same programs through the assert image. Answers are not re-compared —
 *  it is the same translator and runtime — only whether an `assert` fired. */
function runTargetDbg(batch: Verified[]): void
{
    writeBatch(BATCH_PATH, batch)
    const r = runQemu(EXEC_ELF_DBG, BATCH_PATH, QEMU_TIMEOUT_MS, BATCH_ADDR_DBG)

    const results = parseResults(r.output)

    if(r.timedOut)
    {
        // The same programs the answer image has already run, in an image
        // that differs only by its assertions — so this should not be
        // reachable, and is worth the same attribution a target hang gets.
        const culprit = batch[results.length]
        report("DBG HANG", culprit?.entry ?? "?", culprit?.seed ?? 0,
            `assert image did not finish (${r.status}), ${results.length} of ${batch.length} reported`, culprit?.gen)
        return
    }

    const at = results.findIndex(x => x.kind === "A")
    if(at < 0) { bump("dbg clean"); return }

    const culprit = batch[at]
    const text = r.output.split("\n").filter(l => l.startsWith("ASSERT ")).slice(0, 2).join("\n  ")
    report("TARGET ASSERT", culprit?.entry ?? "?", culprit?.seed ?? 0,
        `${text}\n  at line ${results[at]!.value}`, culprit?.gen)
}

const EXEC_ELF_COV = path.join(HERE, "..", "src", "qemu-exec", "exec_runner_cov.elf")

/** Coverage costs its own boot: the answer image is deliberately the
 *  uninstrumented one (`src/qemu-exec/Makefile` keeps it byte-for-byte what
 *  `test/qemu` validates), so what it reaches cannot be read off it. Results
 *  are ignored here — this image is only asked what it touched. */
function runTargetCov(batch: Verified[]): void
{
    writeBatch(BATCH_PATH, batch)
    const r = runQemu(EXEC_ELF_COV, BATCH_PATH, QEMU_TIMEOUT_MS, BATCH_ADDR_DBG)
    coverageBits += unionCoverage(r.output, coverage)
}

function runHost(batch: Verified[]): void
{
    writeBatch(BATCH_PATH, batch)
    const { spawnSync } = require("child_process") as typeof import("child_process")
    const r = spawnSync(HOST_DRIVER, [BATCH_PATH], {encoding: "utf8", timeout: 120_000, maxBuffer: 64 * 1024 * 1024})

    if(r.status === 0) { bump("host clean"); return }

    // A crash under ASan/UBSan, or an assert: the whole batch is suspect, so
    // it is bisected down to the one program responsible.
    const blame = bisect(batch)
    report("CRASH", blame?.entry ?? "?", blame?.seed ?? 0,
        (r.stderr ?? "").split("\n").filter(l => l.length > 0).slice(0, 6).join("\n  "), blame?.gen)
}

/** Which program in a crashing batch is the one that crashes. */
function bisect(batch: Verified[]): Verified | undefined
{
    const { spawnSync } = require("child_process") as typeof import("child_process")
    const crashes = (part: Verified[]): boolean =>
    {
        if(part.length === 0) return false
        writeBatch(BATCH_PATH, part)
        return spawnSync(HOST_DRIVER, [BATCH_PATH], {encoding: "utf8", timeout: 120_000}).status !== 0
    }

    let remaining = batch
    while(remaining.length > 1)
    {
        const half = remaining.slice(0, Math.floor(remaining.length / 2))
        remaining = crashes(half) ? half : remaining.slice(Math.floor(remaining.length / 2))
    }
    return remaining[0]
}

// ── main ────────────────────────────────────────────────────────────────

if(USE_QEMU && !fs.existsSync(EXEC_ELF)) { console.error(`no ${EXEC_ELF} — run "make -C src/qemu-exec"`); process.exit(1) }
if(USE_QEMU && USE_DBG && !fs.existsSync(EXEC_ELF_DBG)) { console.error(`no ${EXEC_ELF_DBG} — run "make -C src/qemu-exec DEBUG=1"`); process.exit(1) }
if(USE_HOST && !fs.existsSync(HOST_DRIVER)) { console.error(`no ${HOST_DRIVER} — run "make -C src/driver"`); process.exit(1) }

// The corpus grows. Without that every mutant is one generation from a
// hand-written seed, and the shapes that only exist several mutations deep —
// a nest inside a nest, a jump table with a loop in one of its cases, a call
// graph three procedures long — are never reached at all: a single pass at
// this mutation rate simply does not build them.
const corpus = seedCorpus()
const CORPUS_MAX = value("--corpus-max", 400)
// A mutation adds far more often than it deletes, so a corpus that fed back
// every novel program would drift monotonically upward until everything in
// it sat against PROGRAM_MAX. Only programs inside this band are fed back,
// which keeps a spread of sizes; a mutant of one may still grow past it and
// be tested, it just does not become the thing the next generation grows
// from.
const CORPUS_MAX_BYTES = value("--corpus-max-bytes", 1024)
/** What a caller other than this file's own CLI supplies. `deadline` and
 *  `stopOnFinding` are the orchestrator's stop conditions; `betweenBatches`
 *  is where it interleaves the lanes. */
export interface CampaignOptions
{
    /** Coverage already established before the loop starts, unioned in so
     *  the report says what the tool reached rather than what the mutants
     *  did. */
    coverage0?: Uint8Array
    rounds?: number
    seed?: number
    /** `Date.now()` past which the loop stops, or undefined for no limit. */
    deadline?: number
    stopOnFinding?: boolean
    /** Called after every batch. Returning false stops the campaign. */
    betweenBatches?: (batches: number) => boolean
    /** Called once per batch with progress, in place of the stderr ticker. */
    onProgress?: (done: number, rounds: number, batches: number, shapes: number, corpusSize: number) => void
}

export interface CampaignResult
{
    counts: Record<string, number>
    findings: readonly Finding[]
    rounds: number
    batches: number
    shapes: number
    corpusSize: number
    coverageBits: number
    coverageBitsTotal: number
    coverage: Uint8Array
    seconds: number
}

export function runCampaign(opts: CampaignOptions = {}): CampaignResult
{
if(opts.rounds !== undefined) ROUNDS = opts.rounds
if(opts.seed !== undefined) SEED0 = opts.seed

// What the seeds reach unmutated is part of what this tool covers, and the
// loop below never runs a pristine seed — every candidate is mutated. Left
// out, a shape only the seed has (a switch whose every case returns, a loop
// body one statement past B's reach) reads as never reached.
if(opts.coverage0 !== undefined)
{
    coverage.set(opts.coverage0.subarray(0, coverage.length))
    for(const b of coverage) for(let i = 0; i < 8; i++) coverageBits += (b >> i) & 1
}

const started = Date.now()
let pool: Verified[] = []
const seen = new Set<string>()
let batches = 0
let generated = 0
let stopped = false

for(let i = 0; i < ROUNDS && !stopped; i++)
{
    const entry = corpus[i % corpus.length]!
    const bulk = i % BULK_EVERY === 0
    const gen = bulk
        ? inflateSequenced(entry.program, SEED0 + i, BULK_SIZES[Math.floor(i / BULK_EVERY) % BULK_SIZES.length]!)
        : mutateSequenced(entry.program, SEED0 + i)
    const verified = verify(entry.name, gen, SEED0 + i)
    if(verified === null) continue
    if(bulk) bump("bulk")

    const novel = !seen.has(verified.signature)
    seen.add(verified.signature)

    // A program with a shape nothing has produced before is worth mutating
    // further, so it joins the corpus. Bounded with replacement, so a long
    // campaign neither grows without limit nor freezes what it explores.
    if(novel && !bulk && verified.bytes.length <= CORPUS_MAX_BYTES)
    {
        const derived = {name: entry.name, program: verified.gen}
        if(corpus.length < CORPUS_MAX) corpus.push(derived)
        else corpus[corpus.length - 1 - (generated++ % (CORPUS_MAX - seedCorpus().length))] = derived
    }

    // The batch spends its slots on shapes it has not run yet rather than on
    // many mutants of one.
    if(!novel && pool.length > BATCH / 2) { bump("shape already covered"); continue }
    pool.push(verified)

    if(pool.length >= BATCH)
    {
        for(const part of chunk(pool))
        {
            if(USE_QEMU) runTarget(part)
            if(USE_QEMU && USE_DBG && batches % DBG_EVERY === 0) runTargetDbg(part)
            if(USE_QEMU && USE_COV && batches % COV_EVERY === 0) runTargetCov(part)
            if(USE_HOST) runHost(part)
        }
        batches++
        pool = []

        if(opts.onProgress !== undefined) opts.onProgress(i + 1, ROUNDS, batches, seen.size, corpus.length)
        else
        {
            // A campaign that prints nothing until it ends looks exactly like
            // a hang, and these run for minutes.
            const largest = Math.max(...corpus.map(c => c.program.procs.reduce((n, x) => n + x.body.length, 0)))
            process.stderr.write(`\r${i + 1}/${ROUNDS} — ${batches} batch(es), ${seen.size} shapes, `
                + `corpus ${corpus.length}, largest ${largest} stmts   `)
        }

        // Stop conditions, checked at a batch boundary so a campaign never
        // ends with candidates generated but never run.
        if(opts.stopOnFinding === true && findings.length > 0) stopped = true
        if(opts.deadline !== undefined && Date.now() >= opts.deadline) stopped = true
        if(opts.betweenBatches !== undefined && !opts.betweenBatches(batches)) stopped = true
    }
}
if(batches > 0 && opts.onProgress === undefined) process.stderr.write("\n")

if(pool.length > 0)
{
    for(const part of chunk(pool))
    {
        if(USE_QEMU) runTarget(part)
        if(USE_QEMU && USE_DBG) runTargetDbg(part)
        if(USE_QEMU && USE_COV) runTargetCov(part)
        if(USE_HOST) runHost(part)
    }
    batches++
}

return {
    counts, findings, rounds: ROUNDS, batches, shapes: seen.size, corpusSize: corpus.length,
    coverageBits, coverageBitsTotal: coverage.length * 8, coverage, seconds: (Date.now() - started) / 1000,
}
}

// ── this file's own CLI, unchanged in behaviour ─────────────────────────

if(require.main === module)
{
    const r = runCampaign()
    console.log(`\n${r.rounds} candidates in ${r.seconds.toFixed(1)}s, ${r.batches} batch(es), `
        + `${r.shapes} distinct shapes, corpus grew to ${r.corpusSize}`)
    for(const [k, v] of Object.entries(r.counts).sort((a, b) => b[1] - a[1])) console.log(`  ${String(v).padStart(6)}  ${k}`)
    if(r.coverageBits > 0) console.log(`\ntarget edge coverage: ${r.coverageBits} of ${r.coverageBitsTotal} bits`)
    console.log(r.findings.length === 0 ? "\nno findings" : `\n${r.findings.length} FINDING(S)`)
    process.exit(r.findings.length === 0 ? 0 : 1)
}
