/* Does the plugin count what it claims to count?
 *
 * Three regions: one around nothing at all, and two around instruction
 * sequences whose executed count and cycle cost are exact by construction.
 * The markers' own instructions fall inside every span equally, so the
 * difference against the empty region is the sequence and nothing else —
 * which is the assertion worth making, and the one that does not depend on
 * knowing the bias.
 *
 * Everything in a measured span is hand-written, the loop count included.
 * Passing it as an argument instead put two compiler-generated instructions
 * inside the region — a Cortex-M0 has no 16-bit immediate move, so
 * `movs`/`lsls` materialized it — which made the expected figure a
 * statement about GCC rather than about the counter. Keeping the count an
 * 8-bit immediate keeps the whole region assembly this file spells out.
 *
 * The two sequences cover the two halves of the cycle model that could each
 * be wrong on their own: the taken-branch pipeline refill, and the memory
 * and multi-register weights.
 */

#include <stdint.h>

#include "../bench_marks.h"
#include "semihost.h"

BENCH_REGION_MARKERS(calibration)
BENCH_REGION_MARKERS(knownloop)
BENCH_REGION_MARKERS(knownmem)

/*   movs r0, #200                      1 insn,   1 cycle
 *   subs r0, #1     x200             200 insns, 200 cycles
 *   bne  1b         x200             200 insns, 199 taken at 3 + 1 untaken at 1
 *                                    ------------------------------------------
 *                                    401 insns, 799 cycles
 *
 * The 199-against-1 split is the whole point: it is what a model that
 * charged every conditional branch the same would get wrong.
 */
static constexpr uint32_t ITERATIONS = 200; // must stay inside movs' 8-bit immediate
static constexpr uint32_t LOOP_INSNS = 2 * ITERATIONS + 1;
static constexpr uint32_t LOOP_CYCLES = 1 + ITERATIONS + (ITERATIONS - 1) * 3 + 1;

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

/*   sub  sp, #8                        1
 *   push {r4}          1 + 1 register = 2
 *   ldr  r4, [sp, #0]                  2
 *   str  r4, [sp, #4]                  2
 *   pop  {r4}          1 + 1 register = 2
 *   add  sp, #8                        1
 *                                     ---
 *   6 insns, 10 cycles
 *
 * The loop above never touches memory, and the memory weights are what
 * actually moved the reported ratios — the JIT materializes pooled literals
 * where C keeps values in registers, so it issues proportionally more
 * loads. A model wrong about `ldr` would be wrong about exactly the thing
 * the benchmark is claiming.
 */
static constexpr uint32_t MEM_INSNS = 6;
static constexpr uint32_t MEM_CYCLES = 10;

static void knownMem()
{
    asm volatile(
        ".syntax unified      \n"
        "   sub  sp, #8       \n"
        "   push {r4}         \n"
        "   ldr  r4, [sp, #0] \n"
        "   str  r4, [sp, #4] \n"
        "   pop  {r4}         \n"
        "   add  sp, #8       \n"
        :
        :
        : "r4", "memory");
}

int main()
{
    bench_enter_calibration();
    bench_exit_calibration();

    bench_enter_knownloop();
    knownLoop();
    bench_exit_knownloop();

    bench_enter_knownmem();
    knownMem();
    bench_exit_knownmem();

    semihostWriteTagged("EXPECT_LOOP_I:", LOOP_INSNS);
    semihostWriteTagged("EXPECT_LOOP_C:", LOOP_CYCLES);
    semihostWriteTagged("EXPECT_MEM_I:", MEM_INSNS);
    semihostWriteTagged("EXPECT_MEM_C:", MEM_CYCLES);
    semihostExit(0);
}
