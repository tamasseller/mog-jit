#ifndef RUNTIME_INTERNAL_H
#define RUNTIME_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <cassert>

#include "resource_codes.h"
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

    CodeArena &memory;        /* owned by whoever runs this program, not by it */
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

    inline Runtime(uint32_t procCount, CodeArena &memory): memory(memory), dispatch(procCount)
    { }

    /* Runs before anything is compiled — the body scan checks itself against
     * stackLimit alone, which is only a floor while the arena is still empty. */
    uint32_t loadProgram(const uint8_t *programBytes, uint32_t programSize, uint32_t bodyOffset);

    inline uint32_t getProcCount() const { return dispatch.getProcCount(); }
    inline ProcSlot &slot(uint32_t idx) { return dispatch.slot(idx); }
    inline const ProcSlot &slot(uint32_t idx) const { return dispatch.slot(idx); }
    inline bool isResident(uint32_t idx) const { return dispatch.isResident(idx); }
    inline void markCompiled(uint32_t idx, uint32_t dest) { dispatch.markCompiled(idx, dest); }
    inline uint32_t sentinelLandingAddress() const { return dispatch.sentinelLandingAddress(); }
    inline uint32_t savedSp() const { return dispatch.savedSp(); }

    inline uint32_t getArenaCursor() const { return memory.getCursor(); }
    inline uint32_t commit(uint32_t newEnd) { return memory.commit(newEnd); }

    uint16_t* ensureSpace(const uint16_t* end, uint32_t lruTick);
};


#endif 
