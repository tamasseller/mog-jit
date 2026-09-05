// jit-armv6m/fuzz — drives exec_runner.elf and does the
// comparison the host-side fuzzer structurally cannot: run the *emitted
// Thumb* and check its answer against mog-core's reference VM.
//
// A miscompilation — a wrong acc fold, a clobbered window register, an
// off-by-one spill offset, a merge point that reads a register the other
// edge never wrote — produces no crash, no assert and no RESOURCE_ERROR.
// It produces the wrong number. Nothing in harness.cpp can see that,
// because nothing there ever executes anything. This is the half that can.
//
// Usage (from the repo root, TS_NODE_PROJECT=jit-armv6m/fuzz/tsconfig.json):
//     npx ts-node --transpile-only jit-armv6m/fuzz/ts/qemu-exec.ts <dir-or-file>...
//
// One QEMU boot per batch, not per program, which is what makes the
// emulator affordable here: the runner streams every program in the batch
// out of one host file over semihosting.

import * as fs from "fs"
import * as path from "path"
import { spawnSync } from "child_process"
import { decodeJitEnvelope, encodeJitProgram, validateProgram, run, StepLimitExceeded, UnspecifiedShiftAmount } from "mog-core"
import type { RtlProgram } from "mog-core"
import { entryArgsFor } from "./lib/entry_args"
// The one extension both halves carry, so an EXT instruction is a
// comparable outcome rather than a bail (rawmem_ext.ts, support/ext-rawmem/ext_rawmem.cpp).
import { rawMemExtension } from "./lib/rawmem_ext"

const EXT = rawMemExtension()

const HERE = path.join(__dirname, "..", "src", "qemu-exec")
const ELF = path.join(HERE, "exec_runner.elf")
const BATCH_PATH = "/tmp/ppl-fuzz-batch.bin"

// All four mirror exec_runner.cpp's own constants.
const BATCH_MAGIC = 0x50504c42 // "PPLB"
const BATCH_ADDR = 0x4000
const BATCH_LIMIT = 0x6000 // flash above the image; the hard per-boot ceiling
const PROGRAM_MAX = 4096

// Generous but not open-ended: a full batch of small programs finishes in
// well under a second, so anything past this is a program that will not
// finish at all. It is also the cost of every hang the sweep steps over,
// so a large value makes a corpus with several hangs unusably slow.
const QEMU_TIMEOUT_MS = 20_000

// A far lower reference-VM watchdog than vm.ts's own 10-million default.
// Classification runs the VM once per input, and *reaching* the default
// limit costs ten million interpreter steps — over a corpus of a few
// hundred thousand programs, the handful that never terminate dominated the
// entire run's wall clock (three minutes of classification before the first
// QEMU boot). A program the reference VM cannot finish in this many steps
// has no reference answer worth a batch slot.
const REFERENCE_MAX_STEPS = 200_000

type Expected =
    | { kind: "return"; acc: number }
    | { kind: "trap"; code: number }

interface Candidate
{
    file: string
    bytes: Buffer
    expected: Expected
    /** The entry procedure's own arguments, as fed to both the reference VM
     *  and the guest — one array, so the two cannot disagree. */
    entryArgs: number[]
}

const skipped: Record<string, number> = {}
function skip(reason: string): void { skipped[reason] = (skipped[reason] ?? 0) + 1 }

function classify(file: string): Candidate | null
{
    const raw = fs.readFileSync(file)
    if(raw.length === 0 || raw.length > PROGRAM_MAX) { skip(`size (>${PROGRAM_MAX}B or empty)`); return null }

    let program: RtlProgram
    try
    {
        const decoded = decodeJitEnvelope(raw, 0, EXT)
        if(decoded.next !== raw.length) { skip("trailing bytes"); return null }
        program = decoded.program
    }
    catch { skip("does not decode"); return null }

    try { validateProgram(program, EXT) }
    catch { skip("does not validate"); return null }

    // Re-encoded, not passed through: encodeJitProgram always writes the
    // max_call_depth/total_depth that validateProgram just recomputed, and
    // the host fuzzer's corpus is full of programs whose envelope stats are
    // whatever a byte flip left behind. That matters here in a way it does
    // not on the host side, because enterProgram* *trusts* those two — it
    // sizes the whole excursion's stack reservation from them and has no
    // way to re-derive them at runtime. One understated total_depth (3
    // where the real depth was 78) had the operand stack run straight down
    // through the reservation into the code arena and hang the emulator:
    // a real "the runtime trusts its input" observation
    // (docs/target-profile.md), but not a translator bug, and not something
    // to spend a 120-second QEMU timeout rediscovering on every such
    // program. Re-encoding keeps the program's own coverage and drops only
    // the forged header.
    let bytes: Buffer
    try { bytes = Buffer.from(encodeJitProgram(program, EXT)) }
    catch { skip("does not re-encode"); return null }
    if(bytes.length === 0 || bytes.length > PROGRAM_MAX) { skip(`size (>${PROGRAM_MAX}B or empty)`); return null }

    // Every entry procedure is runnable: enterProgram* takes the whole
    // argument vector and the batch record carries it per program. Skipping
    // multi-argument entries hides a deterministic hang at five or more
    // (docs/fuzzing-campaign.md).
    const entryArgs = entryArgsFor(program.procedures[0]!.argCount)

    let expected: Expected
    try
    {
        EXT.reset() // the target zeroes its own buffer per program
        const result = run(program, EXT, entryArgs, REFERENCE_MAX_STEPS)

        // A trap at any call depth is comparable: runtime.S's trapHelper
        // unwinds the whole excursion and reports through
        // ProgramResult::trapped's own LANDING_TRAP tag, so a nested trap
        // means here exactly what it means to the reference VM. This used
        // to be set aside — the translator compiled TRAP as an ordinary
        // return, which handed a nested trap's code to its caller as a
        // return value.
        //
        // A void entry procedure's result, on the other hand, is
        // unspecified (isa-core.md §8.7): the reference VM reports whatever
        // it happened to hold and any other backend is free to disagree.
        if(result.ok && !result.accLive) { skip("void entry procedure (§8.7)"); return null }

        expected = result.ok
            ? { kind: "return", acc: result.acc >>> 0 }
            : { kind: "trap", code: (result.trapCode ?? 0) >>> 0 }
    }
    catch(e)
    {
        // Non-terminating programs are legal (isa-core.md §9) — and fatal
        // to batch, since the emulator would spin on one forever and take
        // every later program in the batch with it.
        if(e instanceof StepLimitExceeded) { skip("non-terminating (§9)"); return null }
        // isa-core.md §4.1 leaves a shift by 32 or more unspecified, and
        // the reference VM refuses to invent a value for one rather than
        // handing over its own five-bit masking as though it were the
        // answer. The translator emits a bare register-form shift, which
        // on ARMv6-M shifts by Rm[7:0] — a different, equally legal
        // result. Nothing to compare, and comparing anyway would
        // manufacture a mismatch on every program the mutator happened to
        // give a large shift amount. Only the dynamic combos reach here:
        // an immediate amount outside 0..31 is a validator error, and
        // those are rejected before this point.
        if(e instanceof UnspecifiedShiftAmount) { skip("shift amount >= 32 (unspecified, §4.1)"); return null }
        skip("reference VM threw"); return null
    }

    return { file, bytes, expected, entryArgs }
}

function collect(targets: string[]): string[]
{
    const out: string[] = []
    for(const t of targets)
    {
        if(fs.statSync(t).isDirectory())
        {
            for(const name of fs.readdirSync(t).sort())
            {
                const p = path.join(t, name)
                if(fs.statSync(p).isFile()) out.push(p)
            }
        }
        else out.push(t)
    }
    return out
}

/** Split into chunks that each fit the flash window above the image — the
 *  batch is loaded straight into guest flash, so BATCH_LIMIT is a hard
 *  ceiling per boot, not a tuning knob. */
function chunk(cands: Candidate[]): Candidate[][]
{
    const chunks: Candidate[][] = []
    let current: Candidate[] = []
    let bytes = 8 // magic + count
    for(const c of cands)
    {
        const need = 4 + 4 + 4 * c.entryArgs.length + c.bytes.length
        if(bytes + need > BATCH_LIMIT && current.length > 0)
        {
            chunks.push(current)
            current = []
            bytes = 8
        }
        current.push(c)
        bytes += need
    }
    if(current.length > 0) chunks.push(current)
    return chunks
}

function writeBatch(cands: Candidate[]): void
{
    const header = Buffer.alloc(8)
    header.writeUInt32LE(BATCH_MAGIC, 0)
    header.writeUInt32LE(cands.length, 4)
    const parts: Buffer[] = [header]
    for(const c of cands)
    {
        // u32 length, u32 argCount, argCount x u32, then the program bytes —
        // exec_runner.cpp's own cursor walk, in that order.
        const prefix = Buffer.alloc(8 + 4 * c.entryArgs.length)
        prefix.writeUInt32LE(c.bytes.length, 0)
        prefix.writeUInt32LE(c.entryArgs.length, 4)
        c.entryArgs.forEach((v, i) => prefix.writeUInt32LE(v >>> 0, 8 + 4 * i))
        parts.push(prefix, c.bytes)
    }
    fs.writeFileSync(BATCH_PATH, Buffer.concat(parts))
}

interface QemuRun { output: string; timedOut: boolean; status: string }

function runQemu(): QemuRun
{
    // -serial none rather than -nographic: -nographic wires the model's own
    // UART to stdio, which would interleave its output with the semihosting
    // lines this parses.
    const r = spawnSync("qemu-system-arm", [
        // -m 64k, where test/qemu passes none at all: this board takes its
        // SRAM size from its own SoC, not from -m, so the guest still sees
        // exactly the same 16KB. What -m does change is the *generic
        // loader's* own cap on a blob it will place — it is
        // load_image_targphys's max_sz, and it is ram_size — which at 8k
        // silently refused any batch over 8192 bytes with nothing but
        // "Cannot load specified image". Raising it is what makes a
        // BATCH_LIMIT-sized batch loadable at all.
        "-M", "microbit", "-m", "64k",
        "-serial", "none", "-monitor", "none", "-display", "none",
        "-semihosting-config", "enable=on,target=native",
        "-kernel", ELF,
        "-device", `loader,file=${BATCH_PATH},addr=0x${BATCH_ADDR.toString(16)},force-raw=true`,
    ], { encoding: "utf8", timeout: QEMU_TIMEOUT_MS, maxBuffer: 64 * 1024 * 1024 })

    // Both streams: QEMU writes semihosting output to *stderr* under
    // `target=native`, alongside its own diagnostics ("Timer with period
    // zero, disabling"), and the tag prefixes are what separate the result
    // lines from those. Reading only stdout returns nothing at all.
    return {
        output: (r.stdout ?? "") + (r.stderr ?? ""),
        timedOut: r.signal !== null || r.error !== undefined,
        status: r.error ? String(r.error) : `signal ${r.signal}, status ${r.status}`,
    }
}

// ── main ────────────────────────────────────────────────────────────────

const targets = process.argv.slice(2)
if(targets.length === 0)
{
    console.error("usage: qemu_exec.ts <dir-or-file>...")
    process.exit(1)
}
if(!fs.existsSync(ELF))
{
    console.error(`no ${ELF} — run "make -C src/qemu-exec" first`)
    process.exit(1)
}

const files = collect(targets)
const candidates: Candidate[] = []
files.forEach((f, i) =>
{
    const c = classify(f)
    if(c) candidates.push(c)
    // Progress, because classifying a large corpus is minutes of silence
    // otherwise and looks indistinguishable from a hang.
    if(files.length > 5000 && (i + 1) % 20000 === 0)
        process.stderr.write(`\rclassifying ${i + 1}/${files.length} — ${candidates.length} runnable`)
})
if(files.length > 5000) process.stderr.write("\n")

console.log(`${files.length} input(s), ${candidates.length} runnable`)
for(const [reason, n] of Object.entries(skipped).sort((a, b) => b[1] - a[1]))
    console.log(`  skipped ${n}: ${reason}`)
if(candidates.length === 0) process.exit(0)

let matched = 0, resourceError = 0, rejected = 0
const resourceExamples: string[] = []
// Bucketed by the RESOURCE_* code the runner printed (runtime_host.h).
// Keyed on the raw value, deliberately: mirroring the table here is a
// second copy to keep in sync, and the class nibble is readable as-is.
const resourceByCode = new Map<number, number>()
const mismatches: string[] = []
const hangs: string[] = []
// A work queue rather than a fixed list: a batch cut short by a hang
// requeues its own tail (see below).
const pending: Candidate[][] = chunk(candidates)
const chunks = pending

const show = (e: Expected) => e.kind === "return"
    ? `RETURN 0x${e.acc.toString(16).padStart(8, "0")}`
    : `TRAP 0x${e.code.toString(16).padStart(8, "0")}`

for(let bi = 0; bi < pending.length; bi++)
{
    const batch = pending[bi]!
    writeBatch(batch)

    const { output, timedOut, status } = runQemu()
    const lines = output.split("\n").map(l => l.trim()).filter(l => l.length > 0)

    if(timedOut)
    {
        // A timeout is itself a finding, and a precise one: the runner works
        // through the batch in order printing one line per program, so the
        // first program it had *not* reported when the clock ran out is the
        // one that hung — emitted code that spins, or that faulted into
        // vectors.S's own `hang`, on a program the reference VM finished.
        //
        // Recorded and stepped over rather than fatal: one hang would
        // otherwise end a sweep of hundreds of batches, and a sweep that
        // stops at its first hang never reaches the mismatches behind it.
        const done = lines.filter(l => /^[RTEX]:/.test(l)).length
        const culprit = done < batch.length ? batch[done]! : null
        console.log(`\nHANG OR FAULT (${status}) after ${done} of ${batch.length} in batch ${bi + 1}`
            + (culprit ? `\n  ${culprit.file}` : ""))
        if(culprit) hangs.push(culprit.file)
        // Requeue whatever the hung program was in front of, so the rest of
        // this batch still gets run.
        if(culprit && done + 1 < batch.length) pending.push(batch.slice(done + 1))
        // `continue`, emphatically not `return`: this is the body of a
        // top-level for loop, and a module's top-level `return` exits the
        // whole module — which silently ended a 808-batch sweep at its
        // first hang, in batch 102.
        continue
    }
    const results = lines.filter(l => /^[RTEX]:/.test(l))

    if(results.length !== batch.length)
    {
        console.error(`\nbatch ${bi + 1}/${pending.length}: ran ${results.length} of ${batch.length} — stopped early`)
        const fatal = lines.find(l => l.startsWith("FATAL"))
        if(fatal) console.error(`  runner said: ${fatal}`)
        else if(results.length < batch.length) console.error(`  STOPPED ON: ${batch[results.length]!.file}`)
        process.exit(1)
    }

    batch.forEach((c, i) =>
    {
        const line = results[i]!
        const kind = line[0]!
        const value = parseInt(line.slice(2), 16) >>> 0

        if(kind === "X") { rejected++; return }
        if(kind === "E")
        {
            // Kept, not just counted: a corpus where most programs come back
            // RESOURCE_ERROR is a harness-tuning signal, and "which ones"
            // is the only way to tell an arena that is too small from a
            // corpus whose programs genuinely cannot fit an 8KB target.
            // The per-code split answers exactly that: 0x524521xx is an
            // arena that wants growing, 0x524531xx and 0x524511xx are
            // programs this target cannot compile at any size.
            resourceError++
            resourceByCode.set(value, (resourceByCode.get(value) ?? 0) + 1)
            if(resourceExamples.length < 5) resourceExamples.push(c.file)
            return
        }
        const actual: Expected = kind === "T" ? { kind: "trap", code: value } : { kind: "return", acc: value }

        const agrees = actual.kind === c.expected.kind
            && (actual.kind === "return"
                ? actual.acc === (c.expected as { acc: number }).acc
                : actual.code === (c.expected as { code: number }).code)

        if(agrees) matched++
        else
        {
            // Printed as found, not only in the summary: this sweep can run
            // for many minutes and can be cut short (by a hang, or by the
            // operator), and a finding that only exists in an end-of-run
            // summary is a finding lost whenever that happens — which is
            // exactly what happened the first time a hang aborted a sweep
            // that had already found three mismatches.
            const text = `  ${c.file}\n      reference VM:  ${show(c.expected)}\n      emitted Thumb: ${show(actual)}`
            mismatches.push(text)
            console.log(`\nMISMATCH\n${text}`)
        }
    })

    process.stderr.write(`\rbatch ${bi + 1}/${pending.length} — ${matched} matched, `
        + `${mismatches.length} mismatched, ${hangs.length} hung`)
}

process.stderr.write("\n")
console.log(`matched            ${matched}`)
console.log(`RESOURCE_ERROR     ${resourceError}   (resource/limit bail — legitimate, not comparable)`)
for(const [code, n] of Array.from(resourceByCode).sort((a, b) => b[1] - a[1]))
    console.log(`  ${code.toString(16).padStart(8, "0")}  ${n}`)
if(resourceExamples.length > 0)
    console.log(`  e.g. ${resourceExamples.join("\n       ")}`)
if(rejected) console.log(`rejected by runner ${rejected}`)
console.log(`HANGS/FAULTS       ${hangs.length}`)
console.log(`MISMATCHES         ${mismatches.length}`)
if(hangs.length > 0) console.log("\nhung or faulted:\n" + hangs.map(h => `  ${h}`).join("\n"))

if(mismatches.length > 0)
{
    console.log("\nall mismatches:\n" + mismatches.join("\n"))
    process.exit(1)
}
if(hangs.length > 0) process.exit(1)
