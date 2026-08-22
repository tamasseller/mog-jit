/* jit-armv6m/compiler/test/qemu — end-to-end proof: the real translator
 * (jit-armv6m/compiler/src/translate_proc.h), reached through the real
 * dispatch/eviction runtime (jit-armv6m/prototype/qemu/runtime_host.cpp,
 * unmodified), actually compiles and runs each of the 7 hand-transcribed
 * fixtures (fixtures.cpp) correctly on real QEMU. Hand-written, not
 * ts-node-generated: nothing here depends on @ppl/machine or the TS
 * translator at all.
 *
 * A generous 400-byte arena (matching abi-dispatch.test.ts's own
 * GENEROUS_ARENA convention) — every fixture's own total compiled size
 * measured well under 110 bytes (jit-armv6m/compiler test build log), so
 * no eviction should ever fire; this slice only needs to prove the
 * translator itself works, not eviction against genuinely-compiled code
 * (out of scope, see the plan's own "explicitly out of scope" list).
 */
#include <stdint.h>
#include "runtime_host.h"
#include "fixtures.h"

extern "C" {
void write_hex_result(uint32_t v);
void write_hex_trap(uint32_t v);
void semihosting_exit(int code);
}

static const uint32_t ARENA_SIZE = 400;
static const FlashProc dummyProcs[8] = {}; /* enter_program's own procs param — never dereferenced on this path */

int main(void)
{
    bool allOk = true;

    for(uint32_t f = 0; f < g_fixtureCount; f++)
    {
        const Fixture &fx = g_fixtures[f];
        g_realProcs = fx.procs;
        g_realProcCount = fx.procCount;

        ProgramResult r = enter_program(0, ARENA_SIZE, dummyProcs, fx.procCount);

        bool ok = (r.trapped != 0) == fx.expectTrapped && r.value == fx.expectValue;
        allOk = allOk && ok;

        if(r.trapped) write_hex_trap(r.value);
        else write_hex_result(r.value);
    }

    semihosting_exit(allOk ? 0 : 1);
    return 0;
}
