#ifndef RUNTIME_INTERNAL_H
#define RUNTIME_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <cassert>

#include "runtime_host.h"
#include "code_arena.h"
#include "dispatch_table.h"

class Runtime;

extern "C" [[noreturn]] void runtimeBail(Runtime *runtime, uint32_t code);

/* One program's whole mutable state: where its code goes (CodeArena) and where
 * each procedure's code ended up (DispatchTable). Neither half needs the
 * other; the few operations that do — eviction, and the stack floor that has
 * to bail through the sentinel — are the ones defined here. */
class Runtime
{
    friend struct RuntimeProbe;

    CodeArena memory;
    DispatchTable dispatch;   /* holds the flexible array member — keep last */

    uint32_t pushStackFloor(uint32_t margin);
    void popStackFloor(uint32_t outerFloor);
    void evict(uint32_t idx, const uint16_t *end);
    inline uint32_t occupiedSizeOf(uint32_t idx) const
    {
        const uint32_t addr = dispatch.addressOf(idx);
        return dispatch.nextResidentAbove(addr, memory.getCursor()) - addr;
    }

public:
    class DynamicStackGuard
    {
        DynamicStackGuard(const DynamicStackGuard&) = delete;
        DynamicStackGuard(DynamicStackGuard&&) = delete;

        void operator =(const DynamicStackGuard&) = delete;
        void operator =(DynamicStackGuard&&) = delete;

        Runtime& r;
        uint32_t outerFloor;

    public:
        inline DynamicStackGuard(Runtime& r, uint32_t margin): r(r), outerFloor(r.pushStackFloor(margin)) { }

        inline ~DynamicStackGuard()
        {
            r.popStackFloor(outerFloor);
        }
    };

    static uint32_t storageBytesFor(uint32_t procCount)
    {
        return (uint32_t)sizeof(Runtime) + (procCount + 1) * (uint32_t)sizeof(ProcSlot);
    }

    inline void* operator new(std::size_t count, void* ptr) noexcept 
    { 
        static_assert(UINTPTR_MAX == 0xFFFFFFFFu, "must compile for 32bit arch");
        return ptr; 
    }

    inline Runtime(uint32_t procCount, uint32_t codeArenaBase, uint32_t codeArenaSize, uint32_t stackLimit, uint32_t arenaOverlapsStack, uint32_t interruptReserve = 0):
        memory(codeArenaBase, codeArenaSize, stackLimit, arenaOverlapsStack, interruptReserve), dispatch(procCount)
    { }

    uint32_t loadProgram(const uint8_t *programBytes, uint32_t programSize, uint32_t bodyOffset);

    inline uint32_t getProcCount() const { return dispatch.getProcCount(); }
    inline ProcSlot &slot(uint32_t idx) { return dispatch.slot(idx); }
    inline const ProcSlot &slot(uint32_t idx) const { return dispatch.slot(idx); }
    inline bool isResident(uint32_t idx) const { return dispatch.isResident(idx); }
    inline void markCompiled(uint32_t idx, uint32_t dest, uint32_t lruTick) { dispatch.markCompiled(idx, dest, lruTick); }
    inline uint32_t sentinelLandingAddress() const { return dispatch.sentinelLandingAddress(); }
    inline uint32_t savedSp() const { return dispatch.savedSp(); }

    inline uint32_t getArenaCursor() const { return memory.getCursor(); }
    inline uint32_t commit(uint32_t newEnd) { return memory.commit(newEnd); }

    uint16_t* ensureSpace(const uint16_t* end, uint32_t lruTick);
};

namespace
{
constexpr uint32_t RESOURCE_CODES[] = {
    RESOURCE_PROGRAM_NO_PROCS, RESOURCE_PROGRAM_BODY_UNTERMINATED,
    RESOURCE_PROGRAM_CALLEE_RANGE, RESOURCE_PROGRAM_ENTRY_ARG_COUNT,
    RESOURCE_PROGRAM_ENTRY_DEPTH, RESOURCE_PROGRAM_EXT_UNKNOWN,
    RESOURCE_PROGRAM_EXT_UNSUPPORTED,
    RESOURCE_PROGRAM_RESERVED_OPCODE,
    RESOURCE_EXHAUSTED_ARENA, RESOURCE_EXHAUSTED_STACK_BUDGET,
    RESOURCE_EXHAUSTED_TRANSLATOR_STACK, RESOURCE_EXHAUSTED_SCAN_STACK,
    RESOURCE_LIMIT_WINDOW_RECLAIM, RESOURCE_LIMIT_SPILL_OFFSET,
    RESOURCE_LIMIT_BRANCH_RANGE, RESOURCE_LIMIT_LOOP_BACK_EDGE,
    RESOURCE_LIMIT_ARG_COUNT, RESOURCE_LIMIT_BODY_BYTES,
    RESOURCE_LIMIT_PROC_COUNT, RESOURCE_LIMIT_RESUME_OFFSET,
};

constexpr bool resourceCodesDistinct()
{
    for(unsigned i = 0; i < sizeof(RESOURCE_CODES) / sizeof(RESOURCE_CODES[0]); i++)
    {
        for(unsigned j = i + 1; j < sizeof(RESOURCE_CODES) / sizeof(RESOURCE_CODES[0]); j++)
        {
            if(RESOURCE_CODES[i] == RESOURCE_CODES[j])
            {
                return false;
            }
        }
        if((RESOURCE_CODES[i] >> 16) != RESOURCE_ERROR_SIGNATURE
            || RESOURCE_ERROR_CLASS(RESOURCE_CODES[i]) == 0
            || (RESOURCE_CODES[i] & 0xffu) != 0)
        {
            return false;
        }
    }
    return true;
}
} // namespace

static_assert(resourceCodesDistinct(),
    "RESOURCE_* codes must be distinct, carry the 0x5245 signature and a class nibble, and leave the low byte zero");

#endif 
