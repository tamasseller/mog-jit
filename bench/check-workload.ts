// Does each workload's DSL body agree with its TypeScript transcription
// under the reference VM? Runs before anything is generated for the target,
// so a disagreement here is a workload bug rather than a codegen one.
//
// Both sample counts are checked, not just one: the benchmark measures the
// two and differences them, so a workload that agreed at one length and not
// the other would produce a per-sample figure with no meaning at all.
//
// Usage: npx ts-node --transpile-only bench/check-workload.ts [name...]

import assert from "node:assert/strict"
import {lowerProgram, validateProgram, run} from "../../packages/machine/src/index"
import {sampStreamExtension, packEvent, SAMP_EVENTS, SAMP_OUT_SAMPLES, OUT_MASK, EVENT_MASK}
    from "./sampstream_ext"
import {BENCH_N1, BENCH_N2} from "./bench-config"
import {WORKLOADS, workloadNamed, type Workload} from "./workloads/index"
import {networkSorts} from "./workloads/median5"

function checkAt(w: Workload, n: number): void
{
    assert.equal(n % w.step, 0, `${w.name}: n=${n} is not a multiple of its step ${w.step}`)

    const ext = sampStreamExtension()
    ext.input.set(w.samples())

    const program = lowerProgram(w.proc(), ext)
    const stats = validateProgram(program, ext)
    const result = run(program, ext, [n], 200_000_000)

    assert.equal(result.ok, true, `${w.name} n=${n}: program trapped: ${result.trapCode}`)

    const wantOut = new Int16Array(SAMP_OUT_SAMPLES)
    const wantEvents = new Uint32Array(SAMP_EVENTS)
    let wantCount = 0

    const wantAcc = w.reference(ext.input, n, {
        trigger(index, kind)
        {
            wantEvents[wantCount & EVENT_MASK] = packEvent(index, kind)
            wantCount++
        },
        out(index, value)
        {
            wantOut[index & OUT_MASK] = value
        },
    })

    assert.equal(result.acc, wantAcc, `${w.name} n=${n}: return value disagrees`)
    assert.equal(ext.eventCount(), wantCount, `${w.name} n=${n}: trigger count disagrees`)

    for(let i = 0; i < SAMP_EVENTS; i++)
    {
        assert.equal(ext.events[i], wantEvents[i], `${w.name} n=${n}: event slot ${i} disagrees`)
    }

    for(let i = 0; i < SAMP_OUT_SAMPLES; i++)
    {
        assert.equal(ext.output[i], wantOut[i], `${w.name} n=${n}: output sample ${i} disagrees`)
    }

    /* A workload whose observable output never changes is measuring a loop
     * that could have been optimized to nothing on either side, and would
     * pass every correctness check while proving nothing. */
    const touchedSomething = wantCount > 0 || wantOut.some(v => v !== 0)
    assert.ok(touchedSomething, `${w.name} n=${n}: produced no triggers and no output at all`)

    console.log(`ok: ${w.name} n=${n}, ${wantCount} triggers, `
        + `totalDepth=${stats.totalDepth}, vmSteps=${result.steps}`)
}

function main(): void
{
    assert.ok(networkSorts(), "median5's sorting network does not sort — checked exhaustively")

    const names = process.argv.slice(2)
    const chosen = names.length > 0 ? names.map(workloadNamed) : WORKLOADS

    for(const w of chosen)
    {
        checkAt(w, BENCH_N1)
        checkAt(w, BENCH_N2)
    }
}

main()
