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
extern const jitc::Proc *g_realProcs;
extern uint32_t g_realProcCount;

struct Fixture {
    const char *name;
    const jitc::Proc *procs;
    uint32_t procCount;
    bool expectTrapped;
    uint32_t expectValue;
};

extern const Fixture g_fixtures[];
extern const uint32_t g_fixtureCount;

#endif // JIT_ARMV6M_COMPILER_TEST_QEMU_FIXTURES_H_
