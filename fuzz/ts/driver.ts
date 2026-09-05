// fuzz — the campaign driver.
//
//     npx ts-node --transpile-only fuzz/ts/driver.ts [--rounds N] [--batch N] [--seed N] [--no-qemu] [--no-host]
//
// TS generates and owns the campaign; the two out-of-process sinks are
// handed batches of programs that already passed everything cheap.
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
//           answers from real emitted Thumb) and to the host translator
//           under ASan/UBSan with asserts live (crashes). Neither sink can
//           see the other's findings.
//
// A finding's repro is `(corpus entry, seed)` — `gen/show.ts` prints it.

import * as fs from "fs"
import * as path from "path"
import { encodeJitProgram, run, validateProgram, StepLimitExceeded, UnspecifiedShiftAmount } from "mog-core"
import type { RtlProgram } from "mog-core"
import { rawMemExtension } from "./lib/rawmem_ext"
import { entryArgsFor } from "./lib/entry_args"
import { chunk, digestOf, parseResults, runQemu, writeBatch, PROGRAM_MAX } from "./lib/batch"
import { lowerGen, seedCorpus } from "./gen/corpus"
import type { GenProgram } from "./gen/corpus"
import { inflateSequenced, mutateSequenced, nestDepth } from "./gen/mutate"
import { print } from "./gen/print"
import { saveProgram } from "./gen/program_file"
import { roundTrip } from "./gen/round_trip"
import { evaluate } from "./eval/ast-eval"

const EXT = rawMemExtension()
const HERE = __dirname
const EXEC_ELF = path.join(HERE, "..", "src", "qemu-exec", "exec_runner.elf")
const HOST_DRIVER_DEFAULT = path.join(HERE, "..", "src", "driver", "fuzz_driver")
// Per process, so several workers can run a campaign at once without
// clobbering each other's batch between writing it and running it.
const BATCH_PATH = `/tmp/ppl-fuzz-driver-batch-${process.pid}.bin`

const MAX_STEPS = 200_000
const QEMU_TIMEOUT_MS = 20_000

// The realistic profile (docs/target-profile.md). These were the gate a
// byte-mutation fuzzer needed, because blind mutation reached shapes no
// producer would ever emit — a procedure declaring 900 arguments, a
// program whose operand stack is deeper than the target's whole
// reservation. A generator does not wander there on its own, so these are
// an assertion rather than a filter: tripping one means the generator has
// drifted outside the profile the target is documented to serve.
const REALISTIC_MAX_ARG_COUNT = 16
const REALISTIC_MAX_PROC_COUNT = 16
const REALISTIC_MAX_TOTAL_DEPTH = 128

// ── options ─────────────────────────────────────────────────────────────

const argv = process.argv.slice(2)
const flag = (name: string): boolean => argv.includes(name)
const value = (name: string, fallback: number): number =>
{
    const at = argv.indexOf(name)
    return at < 0 ? fallback : Number(argv[at + 1])
}

const ROUNDS = value("--rounds", 2000)
const BATCH = value("--batch", 200)
const SEED0 = value("--seed", 1)
// The per-batch budget for checking `parse(print(ast))` against `ast`.
// Parsing is, by measurement, the whole cost of the inner loop — an order of
// magnitude more than lowering, the thing actually under test — so the
// property is checked out of band over the corpus rather than per candidate.
// The corpus is the better population for it anyway: it is where the shapes
// that only exist several mutations deep accumulate.
//
// A budget in milliseconds rather than in programs, because the PEG parser
// costs roughly 2x per level of expression nesting: a body of height 13
// parses in 700ms where one of height 4 takes 0.4ms, at the same length. A
// count would spend anything between a millisecond and several seconds.
const VERIFY_MS = value("--verify-ms", 250)
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
const BULK_EVERY = value("--bulk-every", 32)
const BULK_SIZES = [80, 160, 240, 320]

const USE_QEMU = !flag("--no-qemu")
const USE_HOST = !flag("--no-host")
// Which build of the host sink a campaign feeds. The default is the
// ASan/UBSan one that hunts crashes; `src/driver/fuzz_driver_cov` is the
// same code built for line coverage, and answers the other question — which
// translator branches a campaign never reaches at all.
const hostAt = argv.indexOf("--host-driver")
const HOST_DRIVER = hostAt < 0 ? HOST_DRIVER_DEFAULT : path.resolve(argv[hostAt + 1]!)

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

const findings: string[] = []

function report(kind: string, entry: string, seed: number, detail: string, gen?: GenProgram): void
{
    let saved = ""
    if(gen !== undefined)
    {
        fs.mkdirSync(SAVE_DIR, {recursive: true})
        const file = path.join(SAVE_DIR, `${kind.toLowerCase().replace(/[^a-z]+/g, "-")}-${entry}-${seed}.json`)
        saveProgram(file, gen)
        saved = `\n  saved ${file}`
    }
    const source = gen === undefined ? ""
        : "\n" + gen.procs.map((p, i) => `--- p${i}(${p.args.join(", ")}) ---\n${print(p.body)}`).join("")
    const text = `${kind}  descended from ${entry}, round seed ${seed}${saved}\n  ${detail}${source}`
    findings.push(text)
    console.log(`\n${text}`)
}

/** Everything that costs no emulator. Returns a program only when the AST
 *  evaluator and the reference VM agree on it. */
function verify(entry: string, gen: GenProgram | undefined, seed: number): Verified | null
{
    if(gen === undefined) { bump("unsequenced"); return null }

    let rtl: RtlProgram
    let stats
    try
    {
        rtl = lowerGen(gen, EXT, false)
        stats = validateProgram(rtl, EXT)
    }
    catch(e)
    {
        // The generator is supposed to produce only well-formed programs, so
        // this is a generator bug rather than a finding — reported, because
        // a rejection rate that climbs means an invariant has drifted.
        bump(`rejected: ${(e as Error).message.split("\n")[0]!.slice(0, 60)}`)
        return null
    }

    if(rtl.procedures.length > REALISTIC_MAX_PROC_COUNT
        || rtl.procedures.some(p => p.argCount > REALISTIC_MAX_ARG_COUNT)
        || stats.totalDepth > REALISTIC_MAX_TOTAL_DEPTH)
    {
        report("OUT OF PROFILE", entry, seed,
            `${rtl.procedures.length} procedure(s), deepest argCount `
            + `${Math.max(...rtl.procedures.map(p => p.argCount))}, totalDepth ${stats.totalDepth}`, gen)
        return null
    }

    let bytes: Buffer
    try { bytes = Buffer.from(encodeJitProgram(rtl, EXT)) }
    catch(e) { bump(`does not encode: ${(e as Error).message.split("\n")[0]!.slice(0, 50)}`); return null }
    if(bytes.length === 0 || bytes.length > PROGRAM_MAX) { bump("too large"); return null }

    const entryArgs = entryArgsFor(rtl.procedures[0]!.argCount)

    EXT.reset()
    const ast = evaluate(gen, entryArgs, EXT, MAX_STEPS)
    const astDigest = digestOf(EXT.mem)

    EXT.reset()
    let vm
    try { vm = run(rtl, EXT, entryArgs, MAX_STEPS) }
    catch(e)
    {
        if(e instanceof StepLimitExceeded) { bump("step limit"); return null }
        if(e instanceof UnspecifiedShiftAmount)
        {
            // Both refusing to answer is agreement (isa-core.md §4.1).
            if(ast.outcome.kind !== "unspecified")
                report("LOWERER", entry, seed, `evaluator answered ${ast.outcome.kind}, reference VM says the shift is unspecified`, gen)
            bump("unspecified")
            return null
        }
        report("VM THREW", entry, seed, (e as Error).message.split("\n")[0]!, gen)
        return null
    }
    const vmDigest = digestOf(EXT.mem)

    if(ast.outcome.kind === "unspecified") { bump("unspecified"); return null }
    if(ast.outcome.kind === "steplimit") { bump("step limit"); return null }

    // A void entry has no specified result (§8.7); the generator closes
    // every body with a valued return, so this should not arise.
    if(vm.ok && !vm.accLive) { bump("void entry"); return null }

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

/** The printer's own property, over as much of the corpus as the budget
 *  reaches. A failure is a parser or printer bug and is a finding in its own
 *  right: the generator reaches the toolchain through source text, so a
 *  printer that means something other than it prints silently tests a
 *  different program than the one reported.
 *
 *  The cursor persists across sweeps, so a campaign walks the whole corpus
 *  rather than re-checking its head. Time overspent on one body is carried
 *  as debt against the next sweeps: a single deeply nested one can cost
 *  several times the whole budget, and stopping at a deadline would let it
 *  blow the campaign's own share rather than the sweep's. */
let sweepAt = 0
let sweepOwed = 0

function sweepRoundTrip(entries: readonly {name: string; program: GenProgram}[]): void
{
    sweepOwed += VERIFY_MS
    for(let n = 0; n < entries.length && sweepOwed > 0; n++)
    {
        const entry = entries[sweepAt++ % entries.length]!
        const started = Date.now()
        for(const p of entry.program.procs)
        {
            try { roundTrip(p.body) }
            catch(e)
            {
                report("ROUND TRIP", entry.name, 0, (e as Error).message.split("\n").slice(0, 4).join("\n  "), entry.program)
                return
            }
        }
        sweepOwed -= Date.now() - started
        bump("round trip")
    }
}

// ── the outer loop ──────────────────────────────────────────────────────

function runTarget(batch: Verified[]): void
{
    writeBatch(BATCH_PATH, batch)
    const r = runQemu(EXEC_ELF, BATCH_PATH, QEMU_TIMEOUT_MS)

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
if(USE_HOST && !fs.existsSync(HOST_DRIVER)) { console.error(`no ${HOST_DRIVER} — run "make -C src/driver"`); process.exit(1) }

// The corpus grows. Without that every mutant is one generation from a
// hand-written seed, and the shapes that only exist several mutations deep —
// a nest inside a nest, a jump table with a loop in one of its cases, a call
// graph three procedures long — are never reached at all: a single pass at
// this mutation rate simply does not build them.
const corpus = seedCorpus()
const CORPUS_MAX = 400
// A mutation adds far more often than it deletes, so a corpus that fed back
// every novel program would drift monotonically upward until everything in
// it sat against PROGRAM_MAX. Only programs inside this band are fed back,
// which keeps a spread of sizes; a mutant of one may still grow past it and
// be tested, it just does not become the thing the next generation grows
// from.
const CORPUS_MAX_BYTES = 1024
const started = Date.now()
let pool: Verified[] = []
const seen = new Set<string>()
let batches = 0
let generated = 0

for(let i = 0; i < ROUNDS; i++)
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
            if(USE_HOST) runHost(part)
        }
        batches++
        pool = []
        sweepRoundTrip(corpus)

        // A campaign that prints nothing until it ends looks exactly like a
        // hang, and these run for minutes.
        const largest = Math.max(...corpus.map(c => c.program.procs.reduce((n, x) => n + x.body.length, 0)))
        process.stderr.write(`\r${i + 1}/${ROUNDS} — ${batches} batch(es), ${seen.size} shapes, `
            + `corpus ${corpus.length}, largest ${largest} stmts   `)
    }
}
if(batches > 0) process.stderr.write("\n")

if(pool.length > 0)
{
    for(const part of chunk(pool))
    {
        if(USE_QEMU) runTarget(part)
        if(USE_HOST) runHost(part)
    }
    batches++
}

sweepRoundTrip(corpus)

const seconds = (Date.now() - started) / 1000
console.log(`\n${ROUNDS} candidates in ${seconds.toFixed(1)}s, ${batches} batch(es), `
    + `${seen.size} distinct shapes, corpus grew to ${corpus.length}`)
for(const [k, v] of Object.entries(counts).sort((a, b) => b[1] - a[1])) console.log(`  ${String(v).padStart(6)}  ${k}`)
console.log(findings.length === 0 ? "\nno findings" : `\n${findings.length} FINDING(S)`)
process.exit(findings.length === 0 ? 0 : 1)
