// jit-armv6m/compiler/test/qemu — the 7 hand-transcribed fixture programs
// (from jit-armv6m/prototype/test/{call,deep-args,rotation,abi-dispatch}
// .test.ts) and the currently-active-fixture globals compile_proc_real.cpp
// reads from. main.cpp sets g_realProcs/g_realProcCount before each
// enter_program call — sequential, single-threaded, never concurrent.
#ifndef JIT_ARMV6M_COMPILER_TEST_QEMU_FIXTURES_H_
#define JIT_ARMV6M_COMPILER_TEST_QEMU_FIXTURES_H_

#include <cstdint>
#include "proc.h"

// The currently active fixture's own procedure table — reassigned by
// main.cpp before every enter_program call.
extern jitc::Proc *g_realProcs;
extern uint32_t g_realProcCount;

struct Fixture {
    const char *name;
    jitc::Proc *procs;
    uint32_t procCount;
    bool expectTrapped;
    uint32_t expectValue;
    uint32_t argIn = 0;       // fed to enter_program as its own argIn — only meaningful when procs[0].argCount >= 1
    uint32_t arenaSize = 400; // generous by default (abi-dispatch.test.ts's own GENEROUS_ARENA convention) — a handful of eviction/resource-error fixtures override this deliberately small
};

// g_fixtures' own Proc::body pointers are filled in by initFixtures() —
// each fixture's Instr[] source (still authored 1:1 with rtl.ts, instr.h's
// own header) is encoded into its own byte scratch buffer once, at
// startup, since Proc::body is now raw wire bytes rather than a
// compile-time-constructible Instr* (jitc::Proc's own header has why).
// Must run before any enter_program call — main.cpp calls it first thing.
void initFixtures();

extern Fixture g_fixtures[];
extern const uint32_t g_fixtureCount;

#endif // JIT_ARMV6M_COMPILER_TEST_QEMU_FIXTURES_H_
