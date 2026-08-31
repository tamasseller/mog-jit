// Writes the smoke program's JIT envelope to a file, so the host translator
// (bench/dump-emitted.sh) can be pointed at real wire bytes.
//
// Usage: npx ts-node --transpile-only bench/gen-envelope.ts <out-file>

import {writeFileSync} from "node:fs"
import {proc, ir, lowerProgram, validateProgram, encodeJitProgram} from "../../packages/machine/src/index"
import {sampStreamExtension} from "./sampstream_ext"

/* One of each op, in the shapes the emitters actually have to handle: a
 * SAMPLE_AT whose result must survive a sibling's evaluation (so one lands
 * at "tos"), an OUT_AT taking a computed index off the stack, and a TRIGGER
 * with a non-zero kind, which is the only path that emits the `adds`. */
const body = ir`
u32 i = 0;
u32 s = 0;
while (i != n)
{
    s = sample_at(i) + sample_at(i - 1);
    out_at(i, s);
    if (s > 1000)
    {
        trigger(3, i);
    }
    i = i + 1;
}
return s;
`

const out = process.argv[2]
if(out === undefined) throw new Error("usage: gen-envelope.ts <out-file>")

const ext = sampStreamExtension()
const program = lowerProgram(proc(["n"], body), ext)
const stats = validateProgram(program, ext)
const bytes = encodeJitProgram(program, ext)

writeFileSync(out, bytes)
console.log(`${out}: ${bytes.length} bytes, totalDepth=${stats.totalDepth} maxCallDepth=${stats.maxCallDepth}`)
