// fuzz — the batch file, and the two sinks that read it.
//
// One artefact, two consumers: `src/qemu-exec/exec_runner.cpp` runs the
// emitted Thumb on the emulated target, `src/driver/fuzz_driver` translates
// the same programs on the host under ASan/UBSan with asserts live. Neither
// can see the other's findings, so a campaign feeds both.
//
// The layout is exec_runner.cpp's own, documented in its header.

import * as fs from "fs"
import { spawnSync } from "child_process"

export const BATCH_MAGIC = 0x50504c42 // "PPLB"
export const BATCH_ADDR = 0x4000
/** The debug image's own, above its larger rom region — `-DPPL_BATCH_ADDR`
 *  in src/qemu-exec/Makefile and `linker_dbg.ld` must agree with this. */
export const BATCH_ADDR_DBG = 0x8000
/** Flash above the image: a hard ceiling per boot, not a tuning knob.
 *  exec_runner.cpp's own constant. */
export const BATCH_LIMIT = 0x20000
export const PROGRAM_MAX = 4096

export interface BatchEntry
{
    bytes: Buffer
    entryArgs: number[]
}

/** Split into chunks that each fit the flash window above the image. */
export function chunk<T extends BatchEntry>(entries: readonly T[]): T[][]
{
    const chunks: T[][] = []
    let current: T[] = []
    let bytes = 8 // magic + count

    for(const e of entries)
    {
        const need = 4 + 4 + 4 * e.entryArgs.length + e.bytes.length
        if(bytes + need > BATCH_LIMIT && current.length > 0)
        {
            chunks.push(current)
            current = []
            bytes = 8
        }
        current.push(e)
        bytes += need
    }
    if(current.length > 0) chunks.push(current)
    return chunks
}

export function writeBatch(path: string, entries: readonly BatchEntry[]): void
{
    const header = Buffer.alloc(8)
    header.writeUInt32LE(BATCH_MAGIC, 0)
    header.writeUInt32LE(entries.length, 4)

    const parts: Buffer[] = [header]
    for(const e of entries)
    {
        // u32 length, u32 argCount, argCount × u32, then the program —
        // exec_runner.cpp's own cursor walk, in that order.
        const prefix = Buffer.alloc(8 + 4 * e.entryArgs.length)
        prefix.writeUInt32LE(e.bytes.length, 0)
        prefix.writeUInt32LE(e.entryArgs.length, 4)
        e.entryArgs.forEach((v, i) => prefix.writeUInt32LE(v >>> 0, 8 + 4 * i))
        parts.push(prefix, e.bytes)
    }
    fs.writeFileSync(path, Buffer.concat(parts))
}

export interface QemuRun { output: string; timedOut: boolean; status: string }

export function runQemu(elf: string, batchPath: string, timeoutMs: number, batchAddr = BATCH_ADDR): QemuRun
{
    // -serial none rather than -nographic: -nographic wires the model's own
    // UART to stdio, which would interleave its output with the semihosting
    // lines this parses.
    const r = spawnSync("qemu-system-arm", [
        // -m 1M, where test/qemu passes none at all: this board takes its
        // SRAM size from its own SoC, not from -m. What -m does change is
        // the generic loader's cap on a blob it will place — at 8k it
        // silently refused any batch over 8192 bytes, and it has to clear
        // BATCH_LIMIT.
        "-M", "microbit", "-m", "1M",
        "-serial", "none", "-monitor", "none", "-display", "none",
        "-semihosting-config", "enable=on,target=native",
        "-kernel", elf,
        "-device", `loader,file=${batchPath},addr=0x${batchAddr.toString(16)},force-raw=true`,
    ], { encoding: "utf8", timeout: timeoutMs, maxBuffer: 64 * 1024 * 1024 })

    // Both streams: QEMU writes semihosting output to *stderr* under
    // `target=native`, alongside its own diagnostics, and the tag prefixes
    // are what separate the result lines from those.
    return {
        output: (r.stdout ?? "") + (r.stderr ?? ""),
        timedOut: r.signal !== null || r.error !== undefined,
        status: r.error ? String(r.error) : `signal ${r.signal}, status ${r.status}`,
    }
}

export interface TargetResult
{
    /** `R` return, `T` bytecode trap, `E` resource bail, `X` rejected,
     *  `A` an assert fired (debug image only), carrying its line number. */
    kind: string
    value: number
    /** FNV-1a-32 over the extension buffer, or null where none was printed. */
    digest: number | null
}

/** Result lines and their digests interleave, so they are read in order. */
export function parseResults(output: string): TargetResult[]
{
    const results: TargetResult[] = []
    for(const raw of output.split("\n"))
    {
        const line = raw.trim()
        if(/^[RTEXA]:/.test(line))
        {
            results.push({kind: line[0]!, value: parseInt(line.slice(2), 16) >>> 0, digest: null})
        }
        else if(line.startsWith("M:") && results.length > 0)
        {
            results[results.length - 1]!.digest = parseInt(line.slice(2), 16) >>> 0
        }
    }
    return results
}

/** The `COV:` hex dump the instrumented image prints per boot, unioned into
 *  `into`. Returns how many bits it added that were not already there —
 *  which is the number to watch: a campaign that adds none has stopped
 *  reaching anywhere new. */
export function unionCoverage(output: string, into: Uint8Array): number
{
    const line = output.split("\n").map(l => l.trim()).find(l => l.startsWith("COV:"))
    if(line === undefined) return 0

    const hex = line.slice(4)
    // Truncating here would under-report coverage and look like a plateau,
    // so the buffer is sized from the image and a mismatch says so.
    if(hex.length > into.length * 2)
        throw new Error(`COV: line is ${hex.length / 2} bytes, the union buffer is ${into.length}`)

    let added = 0
    for(let i = 0; i * 2 + 1 < hex.length && i < into.length; i++)
    {
        const byte = parseInt(hex.slice(i * 2, i * 2 + 2), 16)
        if(Number.isNaN(byte)) break
        const fresh = byte & ~into[i]!
        for(let b = 0; b < 8; b++) if((fresh >> b) & 1) added++
        into[i] = into[i]! | byte
    }
    return added
}

/** Matches exec_runner.cpp's `rawMemDigest`. */
export function digestOf(mem: Uint8Array): number
{
    let h = 2166136261
    for(const b of mem) { h ^= b; h = Math.imul(h, 16777619) }
    return h >>> 0
}
