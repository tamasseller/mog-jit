// Reference-half smoke check for the sample-stream extension: does a DSL
// body using all three ops lower, validate, and run to the values a plain
// TypeScript transcription of the same loop produces?
//
// This is the gate the target half is written against — the C++ emitters
// have nothing to be checked *for* until the reference semantics are pinned
// down here. Run with:
//   npx ts-node --transpile-only bench/check-ext.ts

import assert from "node:assert/strict"
import {proc, ir, lowerProgram, validateProgram, run} from "../../packages/machine/src/index"
import {sampStreamExtension, packEvent, SAMP_EVENTS, SAMP_OUT_SAMPLES as SAMP_OUT,
    IN_MASK, OUT_MASK, EVENT_MASK}
    from "./sampstream_ext"

const N = 64
const THRESHOLD = 1000

/* Exercises every op and, deliberately, both output locations: the sum is a
 * binary expression whose two operands must tile one to acc and one to tos,
 * which is only reachable through the `:tos` rule variant. */
const body = ir`
u32 i = 0;
u32 s = 0;
while (i != n)
{
    s = sample_at(i) + sample_at(i - 1);
    out_at(i, s);
    if (s > ${THRESHOLD})
    {
        trigger(3, i);
    }
    i = i + 1;
}
return s;
`

/* The event sink is a ring whose count is deliberately *not* masked, so the
 * reference has to model the wrap rather than collect a flat list: with this
 * fixture the loop triggers far more often than SAMP_EVENTS, and comparing a
 * wrapped ring against an unwrapped list compares different events. */
function reference(input: Int16Array): {out: Int16Array; events: Uint32Array; count: number; acc: number}
{
    const out = new Int16Array(SAMP_OUT)
    const events = new Uint32Array(SAMP_EVENTS)
    let count = 0
    let s = 0

    for(let i = 0; i < N; i++)
    {
        // Both reads sign-extend, then the add wraps at 32 bits — exactly
        // what LDRSH followed by ADDS does, and what the VM's own >>> 0 does.
        s = ((input[i & IN_MASK]! + input[(i - 1) & IN_MASK]!) >>> 0)
        out[i & OUT_MASK] = s | 0

        if(s > THRESHOLD)
        {
            events[count & EVENT_MASK] = packEvent(i, 3)
            count++
        }
    }

    return {out, events, count, acc: s}
}

function main(): void
{
    const ext = sampStreamExtension()

    // A shape with real negatives, real wrap-around at the mask, and values
    // both sides of the threshold.
    for(let i = 0; i < ext.input.length; i++)
    {
        ext.input[i] = Math.round(3000 * Math.sin(i / 7)) - 400
    }

    const entry = proc(["n"], body)
    const program = lowerProgram(entry, ext)
    const stats = validateProgram(program, ext)

    const result = run(program, ext, [N])
    assert.equal(result.ok, true, `program trapped: ${result.trapCode}`)

    const want = reference(ext.input)

    assert.equal(result.acc, want.acc, "accumulator disagrees with the reference loop")

    for(let i = 0; i < N; i++)
    {
        assert.equal(ext.output[i & OUT_MASK], want.out[i & OUT_MASK], `output[${i}] disagrees`)
    }

    assert.equal(ext.eventCount(), want.count, "trigger count disagrees")

    for(let i = 0; i < SAMP_EVENTS; i++)
    {
        assert.equal(ext.events[i], want.events[i], `event ring slot ${i} disagrees`)
    }

    assert.ok(want.count > SAMP_EVENTS,
        "the fixture never wraps the event ring — it is not testing the wrap")

    console.log(`ok: ${N} samples, ${want.count} triggers, `
        + `totalDepth=${stats.totalDepth} maxCallDepth=${stats.maxCallDepth}, steps=${result.steps}`)
}

main()
