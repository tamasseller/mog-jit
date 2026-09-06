// fuzz — the frame lane.
//
//     npx ts-node --transpile-only fuzz/ts/frame.ts [--rounds N] [--seed N]
//
// §1.1's frame is the whole binding between the validator and the JIT, and
// since design.md §12 turned every other wire-validity check into an assert
// it is the only runtime check left that stands between a corrupt buffer and
// a translator that trusts its input. So it gets a lane of its own: take a
// program the valid lane accepted, damage it, and require the target to come
// back `RESOURCE_PROGRAM_FRAME` and nothing else.
//
// A folded 16-bit hash misses roughly one corruption in 65536 by
// construction. Those are computed here rather than discovered on the
// target: a damaged program whose frame still verifies is *not* put in the
// batch at all, because running one means translating garbage, which
// design.md §12 makes undefined. They are counted and reported instead.

import * as fs from "fs"
import * as path from "path"
import { encodeJitProgram, programFrameHash, validateProgram, PROGRAM_FRAME_BYTES } from "mog-core"
import { rawMemExtension } from "./lib/rawmem_ext"
import { entryArgsFor } from "./lib/entry_args"
import { chunk, parseResults, runQemu, writeBatch, PROGRAM_MAX } from "./lib/batch"
import { lowerGen, seedCorpus } from "./gen/corpus"
import { mutateSequenced } from "./gen/mutate"
import { Rng } from "./gen/rng"

const EXT = rawMemExtension()
const EXEC_ELF = path.join(__dirname, "..", "src", "qemu-exec", "exec_runner.elf")
const BATCH_PATH = `/tmp/ppl-fuzz-frame-${process.pid}.bin`
const QEMU_TIMEOUT_MS = 20_000
const RESOURCE_PROGRAM_FRAME = 0x52451400

const argv = process.argv.slice(2)
const value = (name: string, fallback: number): number =>
{
    const at = argv.indexOf(name)
    return at < 0 ? fallback : Number(argv[at + 1])
}
const ROUNDS = value("--rounds", 400)
const SEED0 = value("--seed", 1)

/** The target's own check, restated: stored frame against the hash over
 *  everything before it. */
function frameOk(b: Buffer): boolean
{
    if(b.length <= PROGRAM_FRAME_BYTES) return false
    const payloadEnd = b.length - PROGRAM_FRAME_BYTES
    return (b[payloadEnd]! | (b[payloadEnd + 1]! << 8)) === programFrameHash(b, payloadEnd)
}

interface Damage { name: string; apply: (b: Buffer, rng: Rng) => Buffer }

const DAMAGE: Damage[] = [
    {name: "bit flip in the payload", apply: (b, rng) =>
    {
        const out = Buffer.from(b)
        const at = rng.int(Math.max(1, b.length - PROGRAM_FRAME_BYTES))
        out[at] = out[at]! ^ (1 << rng.int(8))
        return out
    }},
    {name: "byte overwritten", apply: (b, rng) =>
    {
        const out = Buffer.from(b)
        const at = rng.int(Math.max(1, b.length - PROGRAM_FRAME_BYTES))
        out[at] = rng.int(256)
        return out
    }},
    {name: "bit flip in the frame", apply: (b, rng) =>
    {
        const out = Buffer.from(b)
        const at = b.length - PROGRAM_FRAME_BYTES + rng.int(PROGRAM_FRAME_BYTES)
        out[at] = out[at]! ^ (1 << rng.int(8))
        return out
    }},
    {name: "truncated", apply: (b, rng) => Buffer.from(b.subarray(0, Math.max(1, b.length - 1 - rng.int(3))))},
    {name: "extended", apply: (b, rng) => Buffer.concat([b, Buffer.from([rng.int(256), rng.int(256)].slice(0, 1 + rng.int(2)))])},
    {name: "two bytes swapped", apply: (b, rng) =>
    {
        const out = Buffer.from(b)
        if(out.length < 3) return out
        const at = rng.int(out.length - 2)
        const t = out[at]!
        out[at] = out[at + 1]!
        out[at + 1] = t
        return out
    }},
    {name: "frame zeroed", apply: b =>
    {
        const out = Buffer.from(b)
        out[out.length - 2] = 0
        out[out.length - 1] = 0
        return out
    }},
]

if(!fs.existsSync(EXEC_ELF)) { console.error(`no ${EXEC_ELF} — run "make -C src/qemu-exec"`); process.exit(1) }

interface Damaged { bytes: Buffer; entryArgs: number[]; damage: string; entry: string; seed: number }

const queued: Damaged[] = []
const collisions: Record<string, number> = {}
const noDamage: Record<string, number> = {}

const corpus = seedCorpus()
for(let i = 0; i < ROUNDS; i++)
{
    const entry = corpus[i % corpus.length]!
    const gen = mutateSequenced(entry.program, SEED0 + i)
    if(gen === undefined) continue

    let bytes: Buffer
    let argCount: number
    try
    {
        const rtl = lowerGen(gen, EXT, false)
        validateProgram(rtl, EXT)
        bytes = Buffer.from(encodeJitProgram(rtl, EXT))
        argCount = rtl.procedures[0]!.argCount
    }
    catch { continue }
    if(bytes.length === 0 || bytes.length > PROGRAM_MAX - 4) continue

    const rng = new Rng(SEED0 + i)
    for(const d of DAMAGE)
    {
        const damaged = d.apply(bytes, rng)
        if(damaged.equals(bytes)) { noDamage[d.name] = (noDamage[d.name] ?? 0) + 1; continue }
        if(frameOk(damaged))
        {
            // The hash missed it. Expected at roughly 2^-16, and the program
            // is *not* run: past the frame it is garbage the translator
            // would take on trust.
            collisions[d.name] = (collisions[d.name] ?? 0) + 1
            continue
        }
        queued.push({bytes: damaged, entryArgs: entryArgsFor(argCount), damage: d.name, entry: entry.name, seed: SEED0 + i})
    }
}

let checked = 0
const wrong: string[] = []

for(const part of chunk(queued))
{
    writeBatch(BATCH_PATH, part)
    const r = runQemu(EXEC_ELF, BATCH_PATH, QEMU_TIMEOUT_MS)
    if(r.timedOut) { wrong.push(`a chunk of ${part.length} hung (${r.status})`); continue }

    const results = parseResults(r.output)
    part.forEach((d, k) =>
    {
        const got = results[k]
        if(got === undefined) { wrong.push(`${d.damage}: no result line (from ${d.entry}, seed ${d.seed})`); return }
        if(got.kind === "E" && got.value === RESOURCE_PROGRAM_FRAME) { checked++; return }
        wrong.push(`${d.damage}: target answered ${got.kind}:${got.value.toString(16)}, `
            + `expected E:${RESOURCE_PROGRAM_FRAME.toString(16)} (from ${d.entry}, seed ${d.seed})`)
    })
}

console.log(`frame lane — ${ROUNDS} bases, ${queued.length} damaged programs\n`)
console.log(`  ${String(checked).padStart(6)}  refused as RESOURCE_PROGRAM_FRAME`)
for(const [k, v] of Object.entries(collisions).sort((a, b) => b[1] - a[1]))
    console.log(`  ${String(v).padStart(6)}  hash collision, not run   ${k}`)
for(const [k, v] of Object.entries(noDamage).sort((a, b) => b[1] - a[1]))
    console.log(`  ${String(v).padStart(6)}  no change                 ${k}`)

const total = Object.values(collisions).reduce((n, v) => n + v, 0)
if(total > 0)
    console.log(`\ncollision rate ${(total / (total + queued.length) * 100).toFixed(3)}% `
        + `(a folded 16-bit frame misses 1 in 65536 ≈ 0.0015%)`)

if(wrong.length > 0)
{
    console.log(`\n${wrong.length} FAILURE(S):`)
    for(const w of wrong.slice(0, 10)) console.log(`  ${w}`)
}
else console.log("\nevery damaged program was refused by the frame")

process.exit(wrong.length === 0 ? 0 : 1)
