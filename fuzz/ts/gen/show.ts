// fuzz — print one mutant, by the pair that identifies it.
//
//     npx ts-node --transpile-only fuzz/ts/gen/show.ts <seed-name> <rng-seed> [--emit <file>]
//
// `--emit` writes the encoded program, which is what `repro.sh` replays
// through the host harness under ASan — the crash sink takes a program, and
// the driver reports only the pair.
//
// That pair plus this module's own version is a finding's whole repro: the
// generator is deterministic in it, so nothing needs saving to disk.

import * as fs from "fs"
import { validateProgram, encodeJitProgram } from "mog-core"
import { rawMemExtension } from "../lib/rawmem_ext"
import { seedCorpus, lowerGen } from "./corpus"
import { mutateSequenced } from "./mutate"
import { print } from "./print"

const EXT = rawMemExtension()
const seeds = seedCorpus()
const [name, seedStr] = process.argv.slice(2)
const entry = seeds.find(s => s.name === name)!
// The same call the pipeline makes, so the pair names the same program.
const gen = mutateSequenced(entry.program, Number(seedStr))
if(gen === undefined) { console.log("no sequenced mutant for this pair"); process.exit(0) }

gen.procs.forEach((p, i) => console.log(`--- p${i}(${p.args.join(", ")}) ---\n${print(p.body)}`))

try
{
    const rtl = lowerGen(gen, EXT)
    console.log("--- rtl ---")
    rtl.procedures.forEach((p, i) => { console.log(` proc ${i} (argCount ${p.argCount})`); p.body.forEach((x, k) => console.log(`   ${k}: ${JSON.stringify(x)}`)) })
    validateProgram(rtl, EXT)
    const bytes = Buffer.from(encodeJitProgram(rtl, EXT))

    const at = process.argv.indexOf("--emit")
    if(at > 0 && process.argv[at + 1] !== undefined)
    {
        fs.writeFileSync(process.argv[at + 1]!, bytes)
        console.log(`--- wrote ${bytes.length} bytes to ${process.argv[at + 1]} ---`)
    }
    else console.log(`--- ok, ${bytes.length} bytes ---`)
}
catch(e) { console.log(`--- FAILED: ${(e as Error).message.split("\n")[0]}`) }
