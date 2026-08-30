// The hand-transcribed fixture programs main.cpp's own fixture loop drives
// Executor::split with. Each Fixture owns one whole, already-encoded
// jit-armv6m program (the packages/machine/src/bytecode.ts envelope plus
// an ordinary isa-core.md §5.5 body) — real wire bytes, the same shape a
// genuine flashed image would have, built once at startup by
// initFixtures() (fixtures.cpp).
#ifndef JIT_ARMV6M_COMPILER_TEST_QEMU_FIXTURES_H_
#define JIT_ARMV6M_COMPILER_TEST_QEMU_FIXTURES_H_

#include <cstdint>

#include "dispatch_abi.h" // LANDING_* — Fixture::expectLanding's own value space

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
    /* procs[0].argCount, recorded by finishProgram rather than restated
     * per fixture row: enterProgram* now requires the argument count it is
     * handed to equal the entry procedure's own declared arg_count
     * exactly, and deriving it here from the same ProcSource the bytes
     * were encoded from is what keeps the two from ever disagreeing. */
    uint32_t entryArgCount;
};

struct Fixture
{
    const char *name;
    const Program *program;
    // One of dispatch_abi.h's LANDING_* tags, compared exactly rather than
    // for truthiness: a bytecode TRAP and a RESOURCE_ERROR are distinct
    // outcomes now, and a fixture that means one must not pass on the
    // other. Every plain-return row spells this `false`, which is 0 —
    // LANDING_SUCCESS.
    uint32_t expectLanding;
    uint32_t expectValue;
    // The entry procedure's sole argument, for the one-argument case that
    // is nearly every row here — main.cpp passes &argIn as a one-element
    // vector when the program declares exactly one. Ignored (but harmless)
    // when it declares none.
    uint32_t argIn = 0;
    // Entry procedures taking two or more arguments point this at their
    // own vector instead; main.cpp prefers it over &argIn when non-null.
    // The count always comes from Program::entryArgCount, never from here.
    const uint32_t *args = nullptr;
    uint32_t arenaSize = 400; // generous by default — no fixture currently overrides it; main.cpp's separate eviction/resource-error TEST cases construct their own tiny programs instead of using this array
};

// Encodes every fixture program into its own scratch slot, since a real
// wire blob (LEB128-encoded) isn't something an Instr[] literal can become
// at compile time on its own. Must run before any Executor::run call —
// main.cpp calls it first thing.
void initFixtures();

extern Fixture fixtures[];
extern const uint32_t fixtureCount;

#endif // JIT_ARMV6M_COMPILER_TEST_QEMU_FIXTURES_H_
