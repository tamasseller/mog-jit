// Workload 1: a pulse-width / window trigger state machine.
//
// The conventional-trigger baseline of the three, and the branchy one: two
// states, hysteresis between two thresholds, a run-length test on the
// falling edge, and almost no arithmetic. This is the class design.md §14's
// "4-6x amortized with control flow" claim is about.
//
// It works on raw unsigned ADC codes, which is both what a real front end
// delivers and the reason this workload is not blocked on the DSL's signed
// types: every comparison here is genuinely unsigned. The samples stay
// inside 0..4095, so the LDRSH the extension emits and the zero-extending
// load a C compiler would pick agree, and neither side is quietly favoured.
//
// Thresholds are compile-time literals on both sides rather than entry
// arguments. That is what a received trigger configuration actually looks
// like — the bytecode arrives with its constants already in it — and it
// keeps the two sides symmetric, since a C reference given the same
// constants folds them the same way.

import {proc, ir, type Procedure} from "../../../../../packages/machine/src/index"
import {SAMP_IN_SAMPLES, IN_MASK} from "../sampstream_ext"
import type {Sink, Workload} from "./workload"

const HI = 2600
const LO = 1400
const MIN_WIDTH = 12
const MAX_WIDTH = 60

const TRIGGER_KIND = 1

function body(): Procedure
{
    return proc(["n"], ir`
u32 state = 0;
u32 run = 0;
for (u32 i = 0; i != n; i++)
{
    u32 s = sample_at(i);
    if (state == 0)
    {
        if (s > ${HI})
        {
            state = 1;
            run = 0;
        }
    }
    else
    {
        run++;
        if (s < ${LO})
        {
            if ((run >= ${MIN_WIDTH}) & (run <= ${MAX_WIDTH}))
            {
                trigger(${TRIGGER_KIND}, i);
            }
            state = 0;
        }
    }
}
return n;
`)
}

/**
 * A pulse train whose widths sweep through and past the accepted band, so
 * both the accept and the reject arm of the width test are exercised, with
 * a slow baseline wander underneath so the hysteresis is doing real work
 * rather than seeing an ideal square wave.
 */
function samples(): Int16Array
{
    const s = new Int16Array(SAMP_IN_SAMPLES)

    let i = 0
    let width = 4

    while(i < s.length)
    {
        const gap = 20 + (width % 7)

        for(let k = 0; k < gap && i < s.length; k++, i++)
        {
            s[i] = 700 + Math.round(220 * Math.sin(i / 53))
        }

        for(let k = 0; k < width && i < s.length; k++, i++)
        {
            s[i] = 3300 + Math.round(220 * Math.sin(i / 53))
        }

        // Sweeps 4..76 and wraps, crossing both MIN_WIDTH and MAX_WIDTH.
        width = width + 6 > 76 ? 4 : width + 6
    }

    return s
}

/**
 * The same machine in plain TypeScript, kept beside the DSL rather than in
 * the generator: this is what the C kernel is a transcription of, and all
 * three have to be read against each other when one of them disagrees.
 */
function reference(input: Int16Array, n: number, sink: Sink): number
{
    let state = 0
    let run = 0

    for(let i = 0; i < n; i++)
    {
        const s = input[i & IN_MASK]! >>> 0

        if(state === 0)
        {
            if(s > HI)
            {
                state = 1
                run = 0
            }
        }
        else
        {
            run = run + 1

            if(s < LO)
            {
                if(run >= MIN_WIDTH && run <= MAX_WIDTH) sink.trigger(i, TRIGGER_KIND)
                state = 0
            }
        }
    }

    return n
}

export const pulseTrigger: Workload = {
    name: "pulse-trigger",
    kernel: "refPulseTrigger",
    params: {
        PULSE_HI: HI,
        PULSE_LO: LO,
        PULSE_MIN_WIDTH: MIN_WIDTH,
        PULSE_MAX_WIDTH: MAX_WIDTH,
        PULSE_TRIGGER_KIND: TRIGGER_KIND,
    },
    proc: body,
    samples,
    reference,
    step: 1,
}
