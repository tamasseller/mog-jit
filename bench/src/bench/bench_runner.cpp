/* The benchmark image: runs each workload twice through the JIT and twice
 * through the compiled reference, over two different sample counts, and
 * reports what it took.
 *
 * Nothing here computes a rate. The image emits raw counts and the host
 * differences them (bench-config.ts explains why): everything that happens
 * once per phase rather than once per sample — translation, Executor setup,
 * the call in and out, the markers' own instructions — is identical at both
 * lengths and cancels, which is what makes a JIT excursion and a compiled
 * function comparable at all.
 *
 * Correctness first, in the same image: both sides write into the same
 * event ring, and the hashes go out with the counts. A measurement taken
 * from two sides that disagree about the answer is not a measurement.
 */

#include <stdint.h>

#include "semihost.h"
#include "executor.h"
#include "bytecode_default.h"
#include "dispatch_abi.h"
#include "runtime.h"
#include "translate_proc.h"

#include "bench_marks.h"
#include "bench_stack.h"
#include "ext_sampstream.h"
#include "generated/bench_data.h"

BENCH_REGION_MARKERS(calibration)
BENCH_REGION_MARKERS(mog_n0)
BENCH_REGION_MARKERS(mog_n1)
BENCH_REGION_MARKERS(mog_n2)
BENCH_REGION_MARKERS(ref_n0)
BENCH_REGION_MARKERS(ref_n1)
BENCH_REGION_MARKERS(ref_n2)

/* 5KB, matching fuzz/src/qemu-exec's own sizing: enough that a benchmark-sized
 * procedure compiles without eviction. Eviction would be a legitimate thing
 * to measure, but not in the same number as steady-state throughput. */
static constexpr uint32_t CODE_ARENA_BYTES = 5120;
alignas(8) static uint8_t g_codeArena[CODE_ARENA_BYTES];

static uint32_t runMog(uint32_t n)
{
    uint32_t entryArgs[1] = {n};

    ProgramResult r = Executor::split(
            (uint32_t)(uintptr_t)g_codeArena, CODE_ARENA_BYTES,
            /*stackLimit=*/(uint32_t)(uintptr_t)(g_codeArena + CODE_ARENA_BYTES),
            /*interruptReserve=*/0)
        .run(bcMapped(g_benchProgram), g_benchProgramLen, entryArgs, 1);

    if (r.trapped)
    {
        /* Both a trap and a resource bail are failures here: this program
         * is fixed, validated, and known to fit. */
        semihostWriteTagged(r.trapped == LANDING_TRAP ? "T:" : "E:", r.value);
        semihostExit(1);
    }

    return r.value;
}

/* FNV-1a over everything a workload can touch: the output stream, then the
 * event ring, then the count. One number covers a filter and a trigger
 * alike, so no workload can quietly go unchecked for lack of a hash that
 * looks at what it writes. The host computes the same walk over the
 * reference VM's own state, so both sides are hashing a value they arrived
 * at independently. */
static uint32_t stateHash()
{
    uint32_t h = 2166136261u;

    for (uint32_t i = 0; i < SAMP_OUT_SAMPLES; i++)
    {
        const uint16_t v = (uint16_t)g_sampOut[i];

        h = (h ^ (v & 0xff)) * 16777619u;
        h = (h ^ ((v >> 8) & 0xff)) * 16777619u;
    }

    for (uint32_t i = 0; i <= SAMP_EVENTS; i++)
    {
        const uint32_t v = i < SAMP_EVENTS ? g_sampEvents.entries[i] : g_sampEvents.count;

        for (uint32_t b = 0; b < 4; b++)
        {
            h = (h ^ ((v >> (8 * b)) & 0xff)) * 16777619u;
        }
    }

    return h;
}

/* What the translator emitted for this program, per procedure, summed.
 *
 * Measured on the target rather than on the host: materializeImm32 picks
 * between a pooled literal and a two-instruction immediate sequence
 * depending on the *value*, and the buffer addresses a host build sees are
 * not the ones the target sees, so a host-side figure could differ by real
 * instructions.
 *
 * A fresh Runtime over a scratch arena, exactly as support/dump-code/dump_code.cpp does,
 * so this measures the program alone and not whatever the benchmark runs
 * already left in the real arena. No bail handling: a program that cannot
 * compile has already failed the phases above.
 */
static uint32_t emittedCodeBytes()
{
    alignas(8) static uint16_t scratch[1024];
    alignas(8) static uint8_t runtimeStorage[sizeof(Runtime) + (BENCH_PROC_COUNT + 1) * sizeof(ProcSlot)];

    uint32_t total = 0;

    for (uint32_t i = 0; i < BENCH_PROC_COUNT; i++)
    {
        for (uint32_t k = 0; k < sizeof(scratch) / sizeof(scratch[0]); k++) scratch[k] = 0;

        CodeArena arena = CodeArena::region((uint32_t)(uintptr_t)scratch, sizeof(scratch),
            /*stackLimit=*/0);
        Runtime &rt = *new (runtimeStorage) Runtime(BENCH_PROC_COUNT, arena);

        BcReader wire;
        wire.open(bcMapped(g_benchProgram + BENCH_BODY_OFFSET), g_benchProgramLen - BENCH_BODY_OFFSET);

        if (rt.loadProgram(wire) != 0)
        {
            semihostWriteTagged("E:", 0xffffffffu);
            semihostExit(1);
        }

        rt.slot(i).lastUsed = 0;
        total += translateProc(i, rt, /*lruTick=*/1) * 2;
    }

    return total;
}

int main()
{
    /* One empty pair, so every other region's marker overhead can be
     * subtracted rather than assumed. */
    bench_enter_calibration();
    bench_exit_calibration();

    sampStreamReset();
    benchPaintStack();
    bench_enter_mog_n0();
    runMog(BENCH_N0);
    bench_exit_mog_n0();
    const uint32_t mogTranslateStack = benchStackUsedBytes();

    sampStreamReset();
    bench_enter_mog_n1();
    runMog(BENCH_N1);
    bench_exit_mog_n1();

    sampStreamReset();
    benchPaintStack();
    bench_enter_mog_n2();
    const uint32_t mogResult = runMog(BENCH_N2);
    bench_exit_mog_n2();

    const uint32_t mogStack = benchStackUsedBytes();
    const uint32_t mogHash = stateHash();

    sampStreamReset();
    bench_enter_ref_n0();
    REF_KERNEL(BENCH_N0);
    bench_exit_ref_n0();

    sampStreamReset();
    bench_enter_ref_n1();
    REF_KERNEL(BENCH_N1);
    bench_exit_ref_n1();

    sampStreamReset();
    benchPaintStack();
    bench_enter_ref_n2();
    const uint32_t refResult = REF_KERNEL(BENCH_N2);
    bench_exit_ref_n2();

    const uint32_t refStack = benchStackUsedBytes();
    const uint32_t refHash = stateHash();

    semihostWriteTagged("MOG_RESULT:", mogResult);
    semihostWriteTagged("REF_RESULT:", refResult);
    semihostWriteTagged("MOG_HASH:", mogHash);
    semihostWriteTagged("REF_HASH:", refHash);
    semihostWriteTagged("MOG_CODE_BYTES:", emittedCodeBytes());
    semihostWriteTagged("MOG_STACK:", mogStack);
    semihostWriteTagged("MOG_TRANSLATE_STACK:", mogTranslateStack);
    semihostWriteTagged("REF_STACK:", refStack);
    semihostWriteTagged("DONE:", 0);

    semihostExit(0);
}
