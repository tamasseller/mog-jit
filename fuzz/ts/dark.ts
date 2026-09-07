// fuzz — every basic block the last campaign never ran.
//
//     npx ts-node --transpile-only fuzz/ts/dark.ts [bitmap]
//
// `fuzz.sh` prints the worst twelve functions and writes the union bitmap
// out; this reads it back and lists all of them, block by block, so the set
// can be worked through without fuzzing again.
import * as fs from "fs"
import * as path from "path"
import { covMap, darkFunctions, loadCoverage } from "./lib/cov_map"

const BITMAP = process.argv[2] ?? "/tmp/ppl-fuzz-coverage.bin"
const ELF = path.join(__dirname, "..", "src", "qemu-exec", "exec_runner_cov.elf")

const map = covMap(ELF)
if(map === null) { console.log("could not read the coverage map from the ELF"); process.exit(1) }

const bits = loadCoverage(BITMAP, ELF)
if(bits === null)
{
    console.log(`${BITMAP}: not there — run ./fuzz/fuzz.sh first, it writes one at the end`)
    process.exit(1)
}
let set = 0
for(const b of bits) for(let i = 0; i < 8; i++) set += (b >> i) & 1

const dark = darkFunctions(map, bits)
console.log(`${set} of ${map.sites.length} blocks ran; ${dark.reduce((n, d) => n + d.dark, 0)} never did\n`)
for(const d of dark)
{
    console.log(`${d.func}  (${d.dark} of ${d.total})`)
    for(const l of d.lines) console.log(`    ${l}`)
}
