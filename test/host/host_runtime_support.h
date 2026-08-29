// The escape a host test unwinds through when the code under test reaches
// Assembler::fail() -> runtimeBail() (see runtime_internal.h's own doc
// comment) -- runtimeBail is [[noreturn]], and host_runtime_support.cpp's
// mock honors that by longjmp-ing here instead of returning, exactly the
// way 1test's own CHECK() failures already unwind a test. A call site
// reached only through fail() must never resume normally, and which
// RESOURCE_* code it reported is the point of the test, so the macro
// below does both halves -- there is no way to write one without the
// other.
#ifndef HOST_RUNTIME_SUPPORT_H_
#define HOST_RUNTIME_SUPPORT_H_

#include "setjmp.h" // Intentionally not <setjmp.h> !

#include "Test.h"

#include "assembler.h"
#include "runtime_internal.h"

#include <cstdint>
#include <cassert>
#include <sys/mman.h>

extern jmp_buf resourceErrorEscape;

#define EXPECT_RESOURCE_ERROR(code, action)                                  \
    do                                                                       \
    {                                                                        \
        MOCK(runtime)::EXPECT(runtimeBail).withParam(code);                   \
        if(!setjmp(resourceErrorEscape))                                      \
        {                                                                    \
            action;                                                          \
            CHECK(false); /* GCOV_EXCL_LINE — unreachable: runtimeBail escapes first */ \
        }                                                                     \
    } while(0)

// Real, dereferenceable memory that also happens to live below 4GB, so its
// address round-trips losslessly through the bare uint32_t fields
// Runtime/ProcSlot use to address a real 32-bit target's flat address
// space (arenaCursor/arenaEnd, bodyPtr). An ordinary 64-bit host process's
// own stack/heap storage doesn't generally fit that (ASLR puts both well
// above 4GB), so every attached-Assembler host test needs this rather
// than a plain local buffer.
class LowMemory
{
    uint8_t *mem;
    uint32_t size;
    uint32_t cursor = 0;
public:
    // Rounded up to a multiple of 4 up front — alloc()'s own rounding
    // means a single allocation for the caller's whole requested size
    // (TestAssembler's own usage below) can round past an unrounded
    // bytes, tripping alloc()'s bounds check spuriously for a caller
    // that asked for a tiny, non-4-aligned region (e.g. 2 bytes).
    explicit LowMemory(uint32_t bytes) : size((bytes + 3u) & ~3u)
    {
        void *p = mmap(nullptr, size, PROT_READ | PROT_WRITE,
            MAP_PRIVATE | MAP_ANONYMOUS | MAP_32BIT, -1, 0);
        assert(p != MAP_FAILED); // GCOV_EXCL_LINE — this file's own setup, not the thing under test
        mem = (uint8_t *)p;
    }
    ~LowMemory() { munmap(mem, size); }
    LowMemory(const LowMemory &) = delete;
    LowMemory &operator=(const LowMemory &) = delete;

    // Bump-allocates bytes (4-aligned, mirroring Runtime::reserveFor's own
    // rounding) and returns its address as the bare uint32_t every
    // ProcSlot::bodyPtr/Runtime::arenaCursor field expects.
    uint32_t alloc(uint32_t bytes)
    {
        uint32_t at = cursor;
        cursor = (cursor + bytes + 3u) & ~3u;
        assert(cursor <= size); // GCOV_EXCL_LINE — this file's own sizing, not the thing under test
        return (uint32_t)(uintptr_t)(mem + at);
    }

    uint8_t *raw(uint32_t addr) const { return (uint8_t *)(uintptr_t)addr; }
    const uint16_t *code(uint32_t addr) const { return (const uint16_t *)(uintptr_t)addr; }
};

// A throwaway single-slot Runtime plus a LowMemory arena, for tests that
// only need a working attached jitc::Assembler (compiler/src/assembler.h)
// — never a real program's procedures/bodies/dispatch. There is no
// longer a detached, buffer-only Assembler(buf, capacity) a host test can
// hand a plain local array to: every Assembler now derives its own
// output buffer straight from a Runtime's own arenaCursor, so this is
// what a test that only wants to emit a few instructions and read them
// back builds one over instead. test_translate_proc.cpp's own
// FakeRuntime is this same idea grown out to a real multi-procedure
// program (bodies, argCounts, needsLRSave); this stays the bare minimum
// for everything that doesn't need that.
class TestAssembler
{
    alignas(8) uint8_t runtimeBytes[sizeof(Runtime) + 2 * sizeof(ProcSlot)] = {};
    LowMemory low;
    uint32_t arenaBase;

    inline Runtime &runtimeRef() { return *reinterpret_cast<Runtime *>(runtimeBytes); }

    // Not resident (Runtime::isResident() reads codePtr against
    // trampolineAddr, never zero) — left at its zero-init default,
    // growForAttached's own findEvictionVictim/evict would see a bogus
    // resident procedure and evict it out from under whatever this
    // Assembler is mid-emitting.
    Runtime &setup(uint32_t capacityHalfwords)
    {
        runtimeRef().procCount = 1;
        arenaBase = low.alloc(capacityHalfwords * 2);
        runtimeRef().arenaCursor = arenaBase;
        runtimeRef().arenaEnd = arenaBase + capacityHalfwords * 2;
        runtimeRef().stackLimit = 0;
        runtimeRef().arenaOverlapsStack = 0;
        runtimeRef().slot(0).codePtr = trampolineAddr;
        return runtimeRef();
    }

public:
    jitc::Assembler a;

    explicit TestAssembler(uint32_t capacityHalfwords)
        : low(capacityHalfwords * 2), a(setup(capacityHalfwords), /*procIdx=*/0, /*lruTick=*/0)
    {
    }

    const uint16_t *code() const { return low.code(arenaBase); }
};

#endif // HOST_RUNTIME_SUPPORT_H_
