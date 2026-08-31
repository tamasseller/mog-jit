// Workload 2: an oversampled IQ preamble detector.
//
// The motivating case, and the one a fixed trigger matrix cannot do at all:
// trigger on a continuous tone rather than on a level or an edge. Sampled
// at four times the tone frequency, the quadrature mixer's coefficients are
// [1, 0, -1, 0] and [0, 1, 0, -1] — every multiply is by 0, +1 or -1, so
// demodulation is add and subtract only. Integrate I and Q over a window,
// then compare I^2 + Q^2 against a squared threshold, which is what avoids
// needing a square root the ISA does not have.
//
// This is the arithmetic-heavy workload, the one design.md §14's "1-3x
// expansion" claim is about, and the only one so far that reaches a MUL.
//
// The accumulators are `i32` and are shifted down before squaring. Both
// facts are load-bearing: a signed `>>` is an ASR, which needs the DSL's
// signed types, and without the shift the square overflows — sixteen groups
// of a 2000-count tone reach |I| ~ 64000, whose square is past 2^32.

import {proc, ir, type Procedure} from "../../../packages/machine/src/index"
import {SAMP_IN_SAMPLES, IN_MASK} from "../sampstream_ext"
import type {Sink, Workload} from "./workload"

/* Groups of four samples integrated before a decision. */
const WINDOW = 16

/* |I| and |Q| reach about 32*A over the window, so a 2000-count tone lands
 * near 64000; >>7 brings that to 500, whose square has ample headroom. */
const SHIFT = 7

/* A tone amplitude of 2000 gives (2000/4)^2 = 250000; 40000 is the square
 * of a quarter of that amplitude, so the detector fires on the bursts and
 * not on the noise floor between them. */
const THRESHOLD_SQ = 40000

const TRIGGER_KIND = 2

const TONE_AMPLITUDE = 2000
const FRAME = 256
const BURST = 128

const PARAMS = {
    IQ_WINDOW: WINDOW,
    IQ_SHIFT: SHIFT,
    IQ_THRESHOLD_SQ: THRESHOLD_SQ,
    IQ_TRIGGER_KIND: TRIGGER_KIND,
}

function body(): Procedure
{
    return proc(["n"], ir`
i32 i = 0;
i32 acci = 0;
i32 accq = 0;
i32 groups = 0;
i32 mi = 0;
i32 mq = 0;
while (i != n)
{
    acci = acci + sample_at(i) - sample_at(i + 2);
    accq = accq + sample_at(i + 1) - sample_at(i + 3);
    groups = groups + 1;
    if (groups == ${WINDOW})
    {
        mi = acci >> ${SHIFT};
        mq = accq >> ${SHIFT};
        if ((mi * mi + mq * mq) > ${THRESHOLD_SQ})
        {
            trigger(${TRIGGER_KIND}, i);
        }
        acci = 0;
        accq = 0;
        groups = 0;
    }
    i = i + 4;
}
return i;
`)
}

/** Tone bursts separated by a low noise floor, so both the detect and the
 *  reject arm run about equally often. */
function samples(): Int16Array
{
    const s = new Int16Array(SAMP_IN_SAMPLES)

    for(let i = 0; i < s.length; i++)
    {
        const inBurst = (i % FRAME) < BURST

        /* A quarter-rate tone is exactly cos(pi*i/2), i.e. the repeating
         * pattern +1, 0, -1, 0 — the thing the mixer is matched to. The
         * phase offset keeps the detector off the degenerate case where all
         * the energy lands in I and none in Q. */
        const tone = inBurst
            ? TONE_AMPLITUDE * Math.cos((Math.PI * i) / 2 + 0.6)
            : 0

        const floor = 60 * Math.sin(i / 3.1) + 40 * Math.sin(i / 11.7)

        s[i] = Math.round(tone + floor)
    }

    return s
}

function reference(input: Int16Array, n: number, sink: Sink): number
{
    let acci = 0
    let accq = 0
    let groups = 0

    for(let i = 0; i !== n; i += 4)
    {
        // `| 0` at each step, matching the machine's 32-bit wrapping.
        acci = (acci + input[i & IN_MASK]! - input[(i + 2) & IN_MASK]!) | 0
        accq = (accq + input[(i + 1) & IN_MASK]! - input[(i + 3) & IN_MASK]!) | 0
        groups++

        if(groups === WINDOW)
        {
            const mi = acci >> SHIFT
            const mq = accq >> SHIFT

            if(((Math.imul(mi, mi) + Math.imul(mq, mq)) | 0) > THRESHOLD_SQ)
            {
                sink.trigger(i, TRIGGER_KIND)
            }

            acci = 0
            accq = 0
            groups = 0
        }
    }

    return n
}

export const iqPreamble: Workload = {
    name: "iq-preamble",
    kernel: "refIqPreamble",
    params: PARAMS,
    proc: body,
    samples,
    reference,
    step: 4,
}
