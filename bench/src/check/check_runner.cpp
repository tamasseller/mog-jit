/* The sample-stream extension's acceptance gate: run one program's emitted
 * Thumb on the real runtime under QEMU and report everything it touched, so
 * the host can compare it against the same program run through
 * mog-core's reference VM.
 *
 * The host dump (bench/dump-emitted.sh) says what the emitters encoded;
 * this says whether executing it means the same thing. Both are needed —
 * an instruction sequence can disassemble exactly as intended and still
 * disagree with the reference about which element it touched.
 *
 * The program and its input samples are baked into flash by
 * bench/ts/gen-check.ts rather than loaded with `-device loader`: there is
 * only ever one program here, and a generated array costs no framing that
 * could itself go wrong.
 */

#include <stdint.h>

#include "semihost.h"
#include "executor.h"
#include "bytecode_default.h"
#include "dispatch_abi.h"

#include "ext_sampstream.h"
#include "generated/check_data.h"

/* 5KB, matching fuzz/src/qemu-exec's own sizing: enough that a benchmark-sized
 * procedure compiles without eviction, leaving the rest of the 16KB for the
 * output ring and the translator's recursion. */
static constexpr uint32_t CODE_ARENA_BYTES = 5120;
alignas(8) static uint8_t g_codeArena[CODE_ARENA_BYTES];

/* FNV-1a over the output ring, byte-wise and little-endian explicitly: the
 * host computes the same walk over an Int16Array, and a memcpy-shaped hash
 * would make the two agree only by accident of endianness. */
static uint32_t outputHash()
{
    uint32_t h = 2166136261u;

    for (uint32_t i = 0; i < SAMP_OUT_SAMPLES; i++)
    {
        const uint16_t v = (uint16_t)g_sampOut[i];

        h = (h ^ (v & 0xff)) * 16777619u;
        h = (h ^ ((v >> 8) & 0xff)) * 16777619u;
    }

    return h;
}

int main()
{
    sampStreamReset();

    /* Executor::run takes its arguments mutably, so they cannot come
     * straight out of flash. */
    uint32_t entryArgs[8];
    for (uint32_t i = 0; i < g_checkArgCount; i++) entryArgs[i] = g_checkArgs[i];

    ProgramResult r = Executor::split(
            (uint32_t)(uintptr_t)g_codeArena, CODE_ARENA_BYTES,
            /*stackLimit=*/(uint32_t)(uintptr_t)(g_codeArena + CODE_ARENA_BYTES),
            /*interruptReserve=*/0)
        .run(bcMapped(g_checkProgram), g_checkProgramLen, entryArgs, g_checkArgCount);

    if (r.trapped == LANDING_TRAP)
    {
        semihostWriteTagged("T:", r.value);
        semihostExit(1);
    }

    if (r.trapped)
    {
        /* A RESOURCE_* bail is a failure here, not a skip: this program is
         * fixed and known to fit. */
        semihostWriteTagged("E:", r.value);
        semihostExit(1);
    }

    semihostWriteTagged("R:", r.value);
    semihostWriteTagged("H:", outputHash());
    semihostWriteTagged("N:", g_sampEvents.count);

    for (uint32_t i = 0; i < SAMP_EVENTS; i++)
    {
        semihostWriteTagged("V:", g_sampEvents.entries[i]);
    }

    semihostWriteTagged("DONE:", 0);
    semihostExit(0);
}
