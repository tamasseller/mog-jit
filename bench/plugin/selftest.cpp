/* Does the plugin count what it claims to count?
 *
 * Two regions: one around nothing at all, one around a loop whose executed
 * instruction count is exact by construction. The markers' own instructions
 * fall inside both spans equally, so the difference between the two counts
 * is the loop and nothing else — which is the assertion worth making, and
 * the one that does not depend on knowing the bias.
 *
 * Everything in the measured span is hand-written, the loop count included.
 * Passing it as an argument instead put two compiler-generated instructions
 * inside the region — a Cortex-M0 has no 16-bit immediate move, so
 * `movs`/`lsls` materialized it — which made the expected figure a
 * statement about GCC rather than about the counter. Keeping the count an
 * 8-bit immediate keeps the whole region assembly this file spells out.
 *
 * `movs` once, then `subs`/`bne` twice per iteration.
 */

#include <stdint.h>

#include "../bench_marks.h"
#include "semihost.h"

BENCH_REGION_MARKERS(calibration)
BENCH_REGION_MARKERS(knownloop)

static constexpr uint32_t ITERATIONS = 200; // must stay inside movs' 8-bit immediate
static constexpr uint32_t EXPECTED_DIFFERENCE = 2 * ITERATIONS + 1;

static void knownLoop()
{
    asm volatile(
        ".syntax unified    \n"
        "   movs r0, %[n]   \n"
        "1: subs r0, #1     \n"
        "   bne  1b         \n"
        :
        : [n] "I"(ITERATIONS)
        : "r0", "cc");
}

int main()
{
    bench_enter_calibration();
    bench_exit_calibration();

    bench_enter_knownloop();
    knownLoop();
    bench_exit_knownloop();

    semihostWriteTagged("EXPECTED_DIFF:", EXPECTED_DIFFERENCE);
    semihostExit(0);
}
