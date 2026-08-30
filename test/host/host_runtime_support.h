// The escape a host test unwinds through when the code under test reaches
// Assembler::fail() -> runtimeBail() (see runtime.h's own doc
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
#include "runtime.h"
#include "runtime_probe.h"

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

class LowMemory
{
    uint8_t *mem;
    uint32_t size;
    uint32_t cursor = 0;
public:
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

class TestAssembler
{
    alignas(8) uint8_t runtimeBytes[sizeof(Runtime) + 2 * sizeof(ProcSlot)] = {};
    LowMemory low;
    uint32_t arenaBase;
    CodeArena arena = CodeArena::region(0, 0, /*stackLimit=*/0);

    inline Runtime &runtimeRef() { return *reinterpret_cast<Runtime *>(runtimeBytes); }

    Runtime &setup(uint32_t capacityHalfwords)
    {
        arenaBase = low.alloc(capacityHalfwords * 2);
        arena = CodeArena::region(arenaBase, capacityHalfwords * 2, /*stackLimit=*/0);
        new(runtimeBytes) Runtime(1, arena);
        RuntimeProbe::setArenaEnd(runtimeRef(), arenaBase + capacityHalfwords * 2);
        runtimeRef().slot(0).codePtr = trampolineAddr;
        return runtimeRef();
    }

public:
    jitc::Assembler a;

    explicit TestAssembler(uint32_t capacityHalfwords)
        : low(capacityHalfwords * 2), a(setup(capacityHalfwords), /*lruTick=*/0)
    {
    }

    const uint16_t *code() const { return low.code(arenaBase); }
};

#endif // HOST_RUNTIME_SUPPORT_H_
