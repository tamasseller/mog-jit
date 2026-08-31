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
 */

#include <stdint.h>

#include "ext_sampstream.h"
#include "kernels_ref.h"
#include "generated/workload_params.h"

namespace
{
/* Mirrors SAMPLE_AT exactly, sign extension included: the emitted LDRSH
 * sign-extends, so a zero-extending read here would differ the moment a
 * workload used a sample below zero, even though this one does not. */
inline uint32_t refSampleAt(uint32_t index)
{
    return (uint32_t)(int32_t)g_sampIn[index & (SAMP_IN_SAMPLES - 1)];
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
        const uint32_t s = refSampleAt(i);

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
