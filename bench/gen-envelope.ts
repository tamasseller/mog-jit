// Writes one workload's JIT envelope to a file, so the host translator
// (bench/dump-emitted.sh) can be pointed at real wire bytes.
//
// Usage: npx ts-node --transpile-only bench/gen-envelope.ts <out-file> [workload]

import {writeFileSync} from "node:fs"
import {lowerProgram, validateProgram, encodeJitProgram} from "../../packages/machine/src/index"
import {sampStreamExtension} from "./sampstream_ext"
import {workloadNamed} from "./workloads/index"

const [out, name = "pulse-trigger"] = process.argv.slice(2)
if(out === undefined) throw new Error("usage: gen-envelope.ts <out-file> [workload]")

const workload = workloadNamed(name)
const ext = sampStreamExtension()
ext.input.set(workload.samples())

const program = lowerProgram(workload.proc(), ext)
const stats = validateProgram(program, ext)
const bytes = encodeJitProgram(program, ext)

writeFileSync(out, bytes)
console.log(`${out}: ${workload.name}, ${bytes.length} bytes, `
    + `totalDepth=${stats.totalDepth} maxCallDepth=${stats.maxCallDepth}`)
