// fuzz — calibrates the coverage map against the image it claims to describe.
//
//     npx ts-node --transpile-only fuzz/ts/cov_check.ts
//
// `cov_rt.cpp` chooses a block's bit and `lib/cov_map.ts` inverts that choice
// to say which blocks a campaign missed. Nothing links the two but agreement,
// and a silent disagreement does not look like a failure — it looks like a
// coverage figure, which is the worst way for this to break. So it is checked
// the way the oracles are: before the number is trusted.
import * as path from "path"
import { encodeJitProgram, validateProgram } from "mog-core"

import { rawMemExtension } from "./lib/rawmem_ext"
import { entryArgsFor } from "./lib/entry_args"
import { writeBatch, runQemu, unionCoverage, BATCH_ADDR_DBG } from "./lib/batch"
import { covMap } from "./lib/cov_map"
import { lowerGen, seedCorpus } from "./gen/corpus"

const EXT = rawMemExtension()
const ELF = path.join(__dirname, "..", "src", "qemu-exec", "exec_runner_cov.elf")
const BATCH = "/tmp/ppl-covcheck-batch.bin"

const fail = (why: string): never => { console.log(`cov map: ${why}`); process.exit(1) }

const map = covMap(ELF)
    ?? fail("could not be read from exec_runner_cov.elf — is arm-none-eabi-objdump on PATH?")

// The index is one bit per four bytes of rom, and every instrumented block
// opens with a four-byte `bl`, so no two can share a bit. If they do, either
// the target's index or this one has moved.
if(map.buckets.size !== map.sites.length)
    fail(`${map.sites.length - map.buckets.size} of ${map.sites.length} blocks collide — the index is not injective`)

if(map.bitmapBytes === 0) fail("g_covBitmap has no size in the ELF symbol table")

const highest = Math.max(...map.sites.map(s => s.bucket))
if(highest >= map.bitmapBytes * 8)
    fail(`a block wants bit ${highest} and g_covBitmap holds ${map.bitmapBytes * 8}`)

// And the target really does set the bits this says it will. One boot: a set
// bit belonging to no known block is the two halves having drifted apart.
const {program} = seedCorpus()[0]!
const rtl = lowerGen(program, EXT, false)
validateProgram(rtl, EXT)
writeBatch(BATCH, [{
    bytes: Buffer.from(encodeJitProgram(rtl, EXT)),
    entryArgs: entryArgsFor(rtl.procedures[0]!.argCount),
}])

const bits = new Uint8Array(map.bitmapBytes)
const set = unionCoverage(runQemu(ELF, BATCH, 60_000, BATCH_ADDR_DBG).output, bits)
if(set === 0) fail("the image reported no coverage at all")

let stray = 0
for(let i = 0; i < bits.length; i++)
    for(let b = 0; b < 8; b++)
        if((bits[i]! >> b) & 1 && !map.buckets.has(i * 8 + b)) stray++

if(stray > 0) fail(`${stray} of ${set} bits the target set belong to no known block`)

console.log(`cov map: ${map.sites.length} blocks, one bit each in ${map.bitmapBytes} bytes; `
    + `all ${set} bits of a live run accounted for`)
