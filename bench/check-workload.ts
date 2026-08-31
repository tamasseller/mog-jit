// Does a workload's DSL body agree with its TypeScript transcription under
// the reference VM? Runs before anything is generated for the target, so a
// disagreement here is a workload bug rather than a codegen one.
//
// Both sample counts are checked, not just one: the benchmark measures the
// two and differences them, so a workload that agreed at one length and not
// the other would produce a per-sample figure with no meaning at all.
//
// Usage: npx ts-node --transpile-only bench/check-workload.ts

import assert from "node:assert/strict"
import {lowerProgram, validateProgram, run} from "../../packages/machine/src/index"
import {sampStreamExtension, packEvent, SAMP_EVENTS, EVENT_MASK} from "./sampstream_ext"
import {pulseTriggerProc, pulseTriggerSamples, pulseTriggerReference, TRIGGER_KIND}
    from "./workloads/pulse-trigger"
import {BENCH_N1, BENCH_N2} from "./bench-config"

function checkAt(n: number): number
{
    const ext = sampStreamExtension()
    ext.input.set(pulseTriggerSamples())

    const program = lowerProgram(pulseTriggerProc(), ext)
    const stats = validateProgram(program, ext)
    const result = run(program, ext, [n], 100_000_000)

    assert.equal(result.ok, true, `n=${n}: program trapped: ${result.trapCode}`)

    const wantEvents = new Uint32Array(SAMP_EVENTS)
    let wantCount = 0

    const wantAcc = pulseTriggerReference(ext.input, n, index =>
    {
        wantEvents[wantCount & EVENT_MASK] = packEvent(index, TRIGGER_KIND)
        wantCount++
    })

    assert.equal(result.acc, wantAcc, `n=${n}: return value disagrees`)
    assert.equal(ext.eventCount(), wantCount, `n=${n}: trigger count disagrees`)

    for(let i = 0; i < SAMP_EVENTS; i++)
    {
        assert.equal(ext.events[i], wantEvents[i], `n=${n}: event ring slot ${i} disagrees`)
    }

    console.log(`ok: pulse-trigger n=${n}, ${wantCount} triggers, `
        + `totalDepth=${stats.totalDepth}, vmSteps=${result.steps}`)

    return wantCount
}

function main(): void
{
    checkAt(BENCH_N1)
    const atN2 = checkAt(BENCH_N2)

    assert.ok(atN2 > SAMP_EVENTS,
        `only ${atN2} triggers at n=${BENCH_N2} — the event ring never wraps, so the `
        + `wrap is untested`)
}

main()
