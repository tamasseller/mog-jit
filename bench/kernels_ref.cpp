/* The compiled reference kernels: the same work the DSL workloads describe,
 * written the way someone would write it directly for the target, and built
 * at every optimization level the suite reports.
 *
 * This is the only translation unit whose optimization level varies. Its
 * accessors are `inline` because the extension's own ops are emitted
 * inline: if the reference reached its samples through an out-of-line call
 * the comparison would be measuring a calling convention rather than the
 * two ways of expressing the loop.
 *
 * C++ rather than C only so ext_sampstream.h can be included as-is. Nothing
 * here uses anything C would not have, and the build gives it
 * -fno-exceptions/-fno-rtti like everything else in the image.
 *
 * Every kernel is `noinline`: it needs its own symbol for the code-size
 * measurement to have something to point at, and inlining it into the
 * runner would put the runner's own code inside the measured region.
 *
 * Signedness is written out rather than left to promotion. The DSL side now
 * picks LT_S/ASR from a variable's declared type, and a kernel that read
 * its samples unsigned would be computing something else — silently, and
 * only on the inputs that go negative.
 */

#include <stdint.h>

#include "ext_sampstream.h"
#include "kernels_ref.h"
#include "generated/workload_params.h"

namespace
{
/* Mirrors SAMPLE_AT: the emitted LDRSH sign-extends, so this is the honest
 * shape and `refSampleAtU` below is the one that needs justifying. */
inline int32_t refSampleAt(uint32_t index)
{
    return g_sampIn[index & (SAMP_IN_SAMPLES - 1)];
}

/* The same load seen as a word, for a workload whose own comparisons are
 * unsigned — pulse-trigger works on raw ADC codes. */
inline uint32_t refSampleAtU(uint32_t index)
{
    return (uint32_t)refSampleAt(index);
}

/* Mirrors OUT_AT, truncation included: the emitted STRH stores the low
 * halfword. */
inline void refOutAt(uint32_t index, int32_t value)
{
    g_sampOut[index & (SAMP_OUT_SAMPLES - 1)] = (int16_t)value;
}

/* Mirrors TRIGGER, packing and unmasked count included. */
inline void refTrigger(uint32_t kind, uint32_t index)
{
    g_sampEvents.entries[g_sampEvents.count & (SAMP_EVENTS - 1)] =
        (index << TRIGGER_KIND_BITS) | (kind & TRIGGER_MAX_KIND);
    g_sampEvents.count++;
}
} // namespace

__attribute__((noinline)) uint32_t refPulseTrigger(uint32_t n)
{
    uint32_t state = 0;
    uint32_t run = 0;
    uint32_t i = 0;

    while (i != n)
    {
        const uint32_t s = refSampleAtU(i);

        if (state == 0)
        {
            if (s > PULSE_HI)
            {
                state = 1;
                run = 0;
            }
        }
        else
        {
            run = run + 1;

            if (s < PULSE_LO)
            {
                /* `&`, not `&&`, matching the DSL body: the machine has no
                 * short-circuit form, and letting the reference skip the
                 * second comparison would be a difference in the work done,
                 * not in how well it was compiled. */
                if ((run >= PULSE_MIN_WIDTH) & (run <= PULSE_MAX_WIDTH))
                {
                    refTrigger(PULSE_TRIGGER_KIND, i);
                }

                state = 0;
            }
        }

        i = i + 1;
    }

    return i;
}

__attribute__((noinline)) uint32_t refIqPreamble(uint32_t n)
{
    int32_t acci = 0;
    int32_t accq = 0;
    int32_t groups = 0;
    uint32_t i = 0;

    while (i != n)
    {
        /* Quadrature mixing at four times the tone frequency: the
         * coefficients are [1,0,-1,0] and [0,1,0,-1], so this is the whole
         * demodulator and there is no multiply in it. */
        acci = acci + refSampleAt(i) - refSampleAt(i + 2);
        accq = accq + refSampleAt(i + 1) - refSampleAt(i + 3);
        groups = groups + 1;

        if (groups == IQ_WINDOW)
        {
            /* Arithmetic shift, and it has to be: the accumulators go
             * negative whenever the tone's phase puts them there. The shift
             * is also what keeps the squares inside 32 bits. */
            const int32_t mi = acci >> IQ_SHIFT;
            const int32_t mq = accq >> IQ_SHIFT;

            if ((mi * mi + mq * mq) > IQ_THRESHOLD_SQ)
            {
                refTrigger(IQ_TRIGGER_KIND, i);
            }

            acci = 0;
            accq = 0;
            groups = 0;
        }

        i = i + 4;
    }

    return i;
}

__attribute__((noinline)) uint32_t refMedian5(uint32_t n)
{
    int32_t a = 0, b = 0, c = 0, d = 0, e = 0, t = 0;
    uint32_t i = 0;

    while (i != n)
    {
        a = refSampleAt(i - 4);
        b = refSampleAt(i - 3);
        c = refSampleAt(i - 2);
        d = refSampleAt(i - 1);
        e = refSampleAt(i);

        /* Knuth's nine-comparator network for five elements, in the same
         * order the DSL body generates it. Cortex-M0 has no IT blocks, so
         * each compare-exchange is a real branch on both sides — neither
         * gets to hide the work in a conditional move. */
        if (a > b) { t = a; a = b; b = t; }
        if (d > e) { t = d; d = e; e = t; }
        if (c > e) { t = c; c = e; e = t; }
        if (c > d) { t = c; c = d; d = t; }
        if (b > e) { t = b; b = e; e = t; }
        if (a > d) { t = a; a = d; d = t; }
        if (a > c) { t = a; a = c; c = t; }
        if (b > d) { t = b; b = d; d = t; }
        if (b > c) { t = b; b = c; c = t; }

        refOutAt(i, c);

        i = i + 1;
    }

    return i;
}
