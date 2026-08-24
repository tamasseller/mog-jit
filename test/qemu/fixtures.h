// The hand-transcribed fixture programs and the currently-active-fixture
// globals compile_proc_real.cpp reads from. main.cpp sets
// realProcs/realProcCount before each enterProgram call — sequential,
// single-threaded, never concurrent.
#ifndef JIT_ARMV6M_COMPILER_TEST_QEMU_FIXTURES_H_
#define JIT_ARMV6M_COMPILER_TEST_QEMU_FIXTURES_H_

#include <cstdint>
#include "proc.h"

// The currently active fixture's own procedure table — reassigned by
// main.cpp before every enterProgram call.
extern jitc::Proc *realProcs;
extern uint32_t realProcCount;

struct Fixture
{
    const char *name;
    jitc::Proc *procs;
    uint32_t procCount;
    bool expectTrapped;
    uint32_t expectValue;
    uint32_t argIn = 0;       // fed to enterProgram as its own argIn — only meaningful when procs[0].argCount >= 1
    uint32_t arenaSize = 400; // generous by default — a handful of eviction/resource-error fixtures override this deliberately small
};

// fixtures' own Proc::body pointers are filled in by initFixtures() — each
// fixture's Instr[] source is encoded into its own byte scratch buffer
// once, at startup, since Proc::body is raw wire bytes rather than a
// compile-time-constructible Instr* (jitc::Proc's own header has why).
// Must run before any enterProgram call — main.cpp calls it first thing.
void initFixtures();

extern Fixture fixtures[];
extern const uint32_t fixtureCount;

#endif // JIT_ARMV6M_COMPILER_TEST_QEMU_FIXTURES_H_
