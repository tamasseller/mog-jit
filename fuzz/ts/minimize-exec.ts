// jit-armv6m/fuzz — shrink a miscompiling program to the
// smallest one that still miscompiles.
//
// Two things make this worth having rather than reusing a byte-level
// delta-debugger:
//
//   * It deletes whole *instructions* and re-encodes (header stats
//     included) through encodeJitProgram, so every candidate it tries is a
//     well-formed, validator-approved program. A byte-level shrinker mostly
//     produces garbage that fails to decode, so it converges on something
//     far larger and much harder to read.
//   * It tests a whole pass's candidates in *one* QEMU boot. The predicate
//     here costs an emulator launch, so testing one variant at a time would
//     make this minutes-per-pass; batching makes a pass one boot.
//
// Two predicates, selected by --hang:
//
//   default   the emitted code's answer disagrees with the reference VM's
//   --hang    the emitted code never finishes (spins, or faults into
//             vectors.S's own `hang`) on a program the reference VM did
//
// Both cost one QEMU boot per pass, including --hang: the runner works
// through a batch in order, so a boot that times out identifies the first
// hanging variant in it by exactly how far it got — which is the same
// information a per-variant probe would give, for one boot instead of one
// per candidate.
//
// Usage (from the repo root, TS_NODE_PROJECT=jit-armv6m/fuzz/tsconfig.json):
//     npx ts-node --transpile-only jit-armv6m/fuzz/ts/minimize-exec.ts [--hang] <file> [out]

import * as fs from "fs"
import * as path from "path"
import { spawnSync } from "child_process"
import { decodeJitEnvelope, encodeJitEnvelope, encodeJitProgram, validateProgram, run, StepLimitExceeded } from "../../../packages/machine/src/index"
import type { RtlProgram } from "../../../packages/machine/src/index"
import { entryArgsFor } from "./lib/entry_args"
import { rawMemExtension } from "./lib/rawmem_ext"

const EXT = rawMemExtension()

const HANG_MODE = process.argv.includes("--hang")

const HERE = path.join(__dirname, "..", "src", "qemu-exec")
const ELF = path.join(HERE, "exec_runner.elf")
const BATCH_PATH = "/tmp/ppl-fuzz-minimize.bin"
const BATCH_MAGIC = 0x50504c42
const BATCH_ADDR = 0x4000
const BATCH_LIMIT = 0x6000
const PROGRAM_MAX = 4096

interface Variant { program: RtlProgram; bytes: Buffer; expected: { trap: boolean; value: number }; entryArgs: number[] }

/** A candidate worth testing at all: encodes canonically, validates, has a
 *  runnable entry procedure, and terminates under the reference VM. */
function prepare(program: RtlProgram): Variant | null
{
    let bytes: Buffer
    try { bytes = Buffer.from(encodeJitProgram(program, EXT)) }
    catch { return null }
    if(bytes.length === 0 || bytes.length > PROGRAM_MAX) return null

    try { validateProgram(program, EXT) } catch { return null }

    // The same vector qemu_exec.ts uses, imported rather than reproduced:
    // a minimizer that fed the guest different arguments than the sweep did
    // would shrink towards a different program than the one that failed.
    const entryArgs = entryArgsFor(program.procedures[0]!.argCount)

    try
    {
        EXT.reset()
        const r = run(program, EXT, entryArgs)
        // Same as the sweep: a void entry procedure's result is
        // unspecified (isa-core.md §8.7), so shrinking towards one would be
        // shrinking towards a disagreement that is not a failure.
        if(r.ok && !r.accLive) return null
        const value = (r.ok ? r.acc : (r.trapCode ?? 0)) >>> 0
        return { program, bytes, expected: { trap: !r.ok, value }, entryArgs }
    }
    catch(e)
    {
        if(e instanceof StepLimitExceeded) return null
        return null
    }
}

/** Run every variant in one boot; return which ones reproduce the failure
 *  being minimized. In --hang mode "reproduces" means the boot stopped
 *  before reporting that variant at all; every variant past it is unknown
 *  (never reached) and so reported false, which is the safe direction — the
 *  minimizer must only shrink to something it positively confirmed. */
function reproduces(variants: Variant[]): boolean[]
{
    if(variants.length === 0) return []

    const header = Buffer.alloc(8)
    header.writeUInt32LE(BATCH_MAGIC, 0)
    header.writeUInt32LE(variants.length, 4)
    const parts: Buffer[] = [header]
    let total = 8
    const included: Variant[] = []
    for(const v of variants)
    {
        // u32 length, u32 argCount, argCount x u32, then the bytes —
        // exec_runner.cpp's record layout, same as qemu_exec.ts writes.
        const prefixBytes = 8 + 4 * v.entryArgs.length
        if(total + prefixBytes + v.bytes.length > BATCH_LIMIT) break
        const prefix = Buffer.alloc(prefixBytes)
        prefix.writeUInt32LE(v.bytes.length, 0)
        prefix.writeUInt32LE(v.entryArgs.length, 4)
        v.entryArgs.forEach((x, i) => prefix.writeUInt32LE(x >>> 0, 8 + 4 * i))
        parts.push(prefix, v.bytes)
        total += prefixBytes + v.bytes.length
        included.push(v)
    }
    header.writeUInt32LE(included.length, 4)
    fs.writeFileSync(BATCH_PATH, Buffer.concat(parts))

    const r = spawnSync("qemu-system-arm", [
        "-M", "microbit", "-m", "64k",
        "-serial", "none", "-monitor", "none", "-display", "none",
        "-semihosting-config", "enable=on,target=native",
        "-kernel", ELF,
        "-device", `loader,file=${BATCH_PATH},addr=0x${BATCH_ADDR.toString(16)},force-raw=true`,
    ], { encoding: "utf8", timeout: HANG_MODE ? 8_000 : 120_000, maxBuffer: 64 * 1024 * 1024 })

    const results = ((r.stdout ?? "") + (r.stderr ?? "")).split("\n")
        .map(l => l.trim()).filter(l => /^[RTEX]:/.test(l))
    const timedOut = r.signal !== null || r.error !== undefined

    if(HANG_MODE)
    {
        // The one variant this boot stopped on — the first it did not
        // report — is the confirmed hang. Nothing else in the batch is
        // known either way.
        return variants.map((_, i) => timedOut && i === results.length && i < included.length)
    }

    // A variant this boot never reached (a hang earlier in the batch, or a
    // short batch) counts as "not reproduced".
    return variants.map((v, i) =>
    {
        const line = results[i]
        if(line === undefined || i >= included.length) return false
        const kind = line[0]!
        if(kind === "E" || kind === "X") return false
        const value = parseInt(line.slice(2), 16) >>> 0
        const trap = kind === "T"
        return trap !== v.expected.trap || value !== v.expected.value
    })
}

// ── main ────────────────────────────────────────────────────────────────

const args = process.argv.slice(2).filter(a => a !== "--hang")
const inFile = args[0]
const outFile = args[1] ?? (inFile ?? "").replace(/\.bin$/, "") + ".min.bin"
if(!inFile) { console.error("usage: minimize_exec.ts [--hang] <file> [out]"); process.exit(1) }

let best = prepare(decodeJitEnvelope(fs.readFileSync(inFile), 0, EXT).program)
if(!best) { console.error("input is not a runnable, comparable program"); process.exit(1) }
if(!reproduces([best])[0])
{
    console.error(HANG_MODE ? "input does not hang" : "input does not miscompile")
    process.exit(1)
}

const count = (p: RtlProgram) => p.procedures.reduce((n, q) => n + q.body.length, 0)
console.log(`start: ${count(best.program)} instructions, ${best.bytes.length} bytes`)

/** Delete one contiguous run of `width` instructions, at every position, in
 *  every procedure. Coarse-to-fine (ddmin's shape) rather than one
 *  instruction at a time: a pass costs a QEMU boot, and in --hang mode a
 *  boot can confirm only a single candidate, so single-instruction passes
 *  alone would need one boot per instruction removed — half an hour for a
 *  180-instruction program. Wide runs collapse most of it in a few passes,
 *  and width 1 finishes the job. */
function candidatesAt(program: RtlProgram, width: number): { variant: Variant; label: string }[]
{
    const out: { variant: Variant; label: string }[] = []
    program.procedures.forEach((proc, pi) =>
    {
        for(let start = 0; start + width <= proc.body.length; start++)
        {
            const body = [...proc.body.slice(0, start), ...proc.body.slice(start + width)]
            const procedures = program.procedures.map((q, k) => k === pi ? { ...q, body } : q)
            const v = prepare({ procedures })
            if(v) out.push({ variant: v, label: `proc ${pi} instr ${start}..${start + width - 1}` })
        }
    })
    return out
}

let width = Math.max(1, Math.floor(count(best.program) / 2))
for(let pass = 1; ; pass++)
{
    const candidates = candidatesAt(best.program, width)
    let bestIdx = -1

    if(candidates.length > 0)
    {
        const verdicts = reproduces(candidates.map(c => c.variant))
        // The smallest surviving candidate, not the first — one pass then
        // makes as much progress as this granularity allows.
        verdicts.forEach((ok, i) =>
        {
            if(!ok) return
            if(bestIdx < 0 || candidates[i]!.variant.bytes.length < candidates[bestIdx]!.variant.bytes.length) bestIdx = i
        })
    }

    if(bestIdx < 0)
    {
        if(width === 1) break
        width = Math.max(1, Math.floor(width / 2))
        continue
    }

    best = candidates[bestIdx]!.variant
    console.log(`pass ${pass} (width ${width}): dropped ${candidates[bestIdx]!.label} -> `
        + `${count(best.program)} instructions, ${best.bytes.length} bytes`)
    if(width > count(best.program)) width = Math.max(1, Math.floor(count(best.program) / 2))
}

// The envelope shape, not `bytes`: `bytes` carries the program frame the
// guest's own `Executor::run` demands, while every reader of a corpus file —
// this minimizer included — takes the unframed envelope.
const out = Buffer.from(encodeJitEnvelope(best.program, EXT))
fs.writeFileSync(outFile, out)
console.log(`\nminimized to ${count(best.program)} instructions, ${out.length} bytes: ${outFile}`)
best.program.procedures.forEach((p, pi) =>
{
    console.log(`  proc ${pi}: argCount=${p.argCount}`)
    p.body.forEach((i, n) => console.log(`    ${n}: ${JSON.stringify(i)}`))
})
console.log(`  reference VM: ${best.expected.trap ? "TRAP" : "RETURN"} 0x${best.expected.value.toString(16)}`
    + (HANG_MODE ? "  (the emitted code never finishes)" : ""))
