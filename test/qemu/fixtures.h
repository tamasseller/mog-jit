// The hand-transcribed fixture programs main.cpp's own fixture loop drives
// enterProgramSplit() with. Each Fixture owns one whole, already-encoded
// jit-armv6m program (the packages/machine/src/bytecode.ts envelope plus
// an ordinary isa-core.md §5.5 body) — real wire bytes, the same shape a
// genuine flashed image would have, built once at startup by
// initFixtures() (fixtures.cpp).
#ifndef JIT_ARMV6M_COMPILER_TEST_QEMU_FIXTURES_H_
#define JIT_ARMV6M_COMPILER_TEST_QEMU_FIXTURES_H_

#include <cstdint>

// One whole encoded program (bytes + length). A plain struct rather than
// two separate Fixture fields: fixtures[] (below) is itself a static
// array, initialized at static-init time — before initFixtures() ever
// runs — so its own initializers can only safely capture each fixture's
// own Program by *address* (fixed at link time, valid immediately) and
// read through it later, never copy its bytes/size *by value* (which
// would freeze in whatever those fields held before initFixtures() filled
// them in — nullptr/0, silently).
struct Program
{
    const uint8_t *bytes;
    uint32_t size;
};

struct Fixture
{
    const char *name;
    const Program *program;
    bool expectTrapped;
    uint32_t expectValue;
    uint32_t argIn = 0;       // fed to enterProgramSplit as its own argIn — only meaningful when procs[0].argCount >= 1
    uint32_t arenaSize = 400; // generous by default — no fixture currently overrides it; main.cpp's separate eviction/resource-error TEST cases construct their own tiny programs instead of using this array
};

// Encodes every fixture program into its own scratch slot, since a real
// wire blob (LEB128-encoded) isn't something an Instr[] literal can become
// at compile time on its own. Must run before any enterProgramSplit call —
// main.cpp calls it first thing.
void initFixtures();

extern Fixture fixtures[];
extern const uint32_t fixtureCount;

#endif // JIT_ARMV6M_COMPILER_TEST_QEMU_FIXTURES_H_
