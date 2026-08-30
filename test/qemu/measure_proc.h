// Builds one encoded procedure body and measures what the real translator
// compiles it to — the only helper test_eviction.cpp and
// test_stack_budget.cpp both need, so it lives here rather than being
// duplicated. Everything else each of those files needs is small enough to
// own locally.
#ifndef JIT_ARMV6M_TEST_QEMU_MEASURE_PROC_H_
#define JIT_ARMV6M_TEST_QEMU_MEASURE_PROC_H_

#include <cstdint>
#include <new>

#include "instr.h"
#include "encode_instr.h"
#include "translate_proc.h"
#include "runtime.h"

namespace jitc
{

struct Proc
{
    uint32_t argCount;
    const uint8_t *body;
    uint32_t bodyBytes;
};

inline Proc makeProc(uint32_t argCount, const Instr *body, uint32_t count, uint8_t *bytesOut, uint32_t bytesCap)
{
    uint32_t len = encodeBody(body, count, bytesOut, bytesCap);
    return Proc{argCount, bytesOut, len};
}

inline uint32_t measuredHalfwords(const Proc &proc, uint32_t procIdx, const uint32_t *calleeArgCounts, uint32_t calleeCount, bool savesLR)
{
    static uint16_t scratch[128];

    alignas(8) uint8_t runtimeBytes[sizeof(Runtime) + (calleeCount + 1) * sizeof(ProcSlot)] = {};
    CodeArena arena = CodeArena::region((uint32_t)(uintptr_t)scratch, sizeof(scratch), /*stackLimit=*/0);
    Runtime &r = *new(runtimeBytes) Runtime(calleeCount, arena);
    for(uint32_t i = 0; i < calleeCount; i++)
    {
        r.slot(i).codePtr = trampolineAddr;
        r.slot(i).setStaticInfo(calleeArgCounts[i], /*bodyBytes=*/0, i == procIdx && savesLR);
    }
    r.slot(procIdx).bodyPtr = (uint32_t)(uintptr_t)proc.body;
    r.slot(procIdx).setStaticInfo(proc.argCount, proc.bodyBytes, savesLR);

    return translateProc(procIdx, r, /*lruTick=*/0);
}

} // namespace jitc

#endif // JIT_ARMV6M_TEST_QEMU_MEASURE_PROC_H_
