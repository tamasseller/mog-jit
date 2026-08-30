#ifndef RUNTIME_INTERNAL_H
#define RUNTIME_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <cassert>
#include "runtime_host.h"
#include "proc_scan.h"
#include "ext.h"
#include "decode_instr.h"
#include "abi_strategy.h"

class Runtime;

extern "C" [[noreturn]] void runtimeBail(Runtime *runtime, uint32_t code);

struct ProcSlot
{
    uint32_t codePtr;    
    uint32_t lastUsed;   
    uint32_t bodyPtr;
    uint32_t staticInfo; 

    static constexpr uint32_t MAX_ARG_COUNT = (1u << 11) - 1;
    static constexpr uint32_t MAX_BODY_BYTES = (1u << 20) - 1;

    inline void setStaticInfo(uint32_t argCount, uint32_t bodyBytes, bool needsLRSave)
    {
        staticInfo = (needsLRSave ? 0x80000000u : 0u) | (argCount << 20) | bodyBytes;
    }

    inline uint32_t argCount() const
    {
        return (staticInfo >> 20) & MAX_ARG_COUNT;
    }

    inline uint32_t bodyBytes() const
    {
        return staticInfo & MAX_BODY_BYTES;
    }

    inline bool needsLRSave() const
    {
        return (staticInfo & 0x80000000u) != 0;
    }

    inline void setCodePtr(uint32_t addr)
    {
        codePtr = addr | 1u;
    }

    inline void slideCodePtr(uint32_t delta)
    {
        codePtr = (codePtr - delta) | 1u;
    }
};

static_assert(sizeof(ProcSlot) == 16, "power-of-two: idx*16 must stay a shift, not a multiply — runtime.S's own hardcoded stride");

extern const uint32_t trampolineAddr;

class Runtime
{
public:
    uint32_t arenaEnd;
    uint32_t arenaCursor;
    uint32_t procCount;
    const ExtHooks *ext;
    uint32_t stackLimit;
    uint32_t arenaOverlapsStack;   /* 0/1, not bool, so this struct's layout is trivial */
    ProcSlot slots[];

public:
    static uint32_t storageBytesFor(uint32_t procCount)
    {
        return (uint32_t)sizeof(Runtime) + (procCount + 1) * (uint32_t)sizeof(ProcSlot);
    }

    inline void* operator new(std::size_t count, void* ptr) noexcept 
    { 
        static_assert(UINTPTR_MAX == 0xFFFFFFFFu, "must compile for 32bit arch");
        return ptr; 
    }

    inline Runtime(uint32_t procCount, uint32_t codeArenaBase, uint32_t codeArenaSize, uint32_t stackLimit, uint32_t arenaOverlapsStack, const ExtHooks *extension = nullptr):
        arenaEnd((codeArenaBase + codeArenaSize) & ~3u), arenaCursor((codeArenaBase + 3u) & ~3u), procCount(procCount), ext(extension), stackLimit(stackLimit), arenaOverlapsStack(arenaOverlapsStack)
    {
        assert(arenaCursor <= arenaEnd); // GCOV_EXCL_LINE

        slots[0].lastUsed = 0;
        slots[0].bodyPtr = 0;
        slots[0].staticInfo = 0;
    }

    uint32_t loadProgram(const uint8_t *programBytes, uint32_t programSize, uint32_t bodyOffset)
    {
        if(this->ext != nullptr && this->ext->abiVersion != jitc::EXT_ABI_VERSION)
        {
            return RESOURCE_PROGRAM_EXT_ABI;
        }

        if(procCount > jitc::MAX_PROC_IDX + 1)
        {
            return RESOURCE_LIMIT_PROC_COUNT;
        }

        uint32_t pos = bodyOffset;
        for(uint32_t i = 0; i < procCount; i++)
        {
            assert(pos < programSize); // GCOV_EXCL_LINE — malformed/truncated program, matching decode_instr.cpp's own convention
            
            uint32_t argCount = jitc::decodeLeb128(programBytes, pos, pos);
            uint32_t bodyStart = pos;

            jitc::BodyScanResult scan = jitc::scanProcBody(programBytes, programSize, bodyStart, this->ext, stackLimit);

            if(!scan.ok)
            {
                return scan.failCode; 
            }
            if(argCount > ProcSlot::MAX_ARG_COUNT)
            {
                return RESOURCE_LIMIT_ARG_COUNT;
            }
            if(scan.bodyBytes > ProcSlot::MAX_BODY_BYTES)
            {
                return RESOURCE_LIMIT_BODY_BYTES;
            }

            ProcSlot &s = slot(i);
            s.setCodePtr(trampolineAddr);
            s.lastUsed = 0;
            s.bodyPtr = (uint32_t)(uintptr_t)(programBytes + bodyStart);
            s.setStaticInfo(argCount, scan.bodyBytes, scan.needsLRSave);

            pos = bodyStart + scan.bodyBytes;
        }

        return 0;
    }

    inline auto getArenaCursor() const 
    {
        return arenaCursor;
    }

    inline auto getProcCount() const 
    {
        return procCount;
    }

    const ExtHooks *extension() const
    {
        return ext;
    }

    ProcSlot &slot(uint32_t idx)
    {
        return slots[idx + 1];
    }

    const ProcSlot &slot(uint32_t idx) const
    {
        return slots[idx + 1];
    }

    uint32_t sentinelLandingAddress() const
    {
        return slots[0].codePtr;
    }

    uint32_t savedSp() const
    {
        return slots[0].bodyPtr;
    }

    bool isResident(uint32_t idx) const
    {
        return slot(idx).codePtr != trampolineAddr;
    }

    bool hasRoomFor(uint32_t need) const
    {
        return arenaEnd - arenaCursor >= need;
    }

    uint32_t liveStackFloor() const
    {
        return (arenaOverlapsStack && arenaCursor > stackLimit) ? arenaCursor : stackLimit;
    }

    uint32_t commit(uint32_t newEnd)
    {
        assert(arenaCursor <= newEnd && newEnd <= arenaEnd);
        
        arenaCursor = (newEnd + 3u) & ~3u;
        
        return arenaCursor;
    }

    void markCompiled(uint32_t idx, uint32_t dest, uint32_t lruTick);
    int findEvictionVictim(uint32_t now) const;
    uint32_t occupiedSizeOf(uint32_t idx) const;
    void evict(uint32_t idx, const uint16_t *end);
    uint16_t* ensureSpace(const uint16_t* end, uint32_t lruTick);
};

static_assert(sizeof(ProcSlot) == DISPATCH_SENTINEL_OFFSET,
    "runtime.S's own DISPATCH_SENTINEL_OFFSET must match sizeof(ProcSlot)");

namespace
{
constexpr uint32_t RESOURCE_CODES[] = {
    RESOURCE_PROGRAM_NO_PROCS, RESOURCE_PROGRAM_BODY_UNTERMINATED,
    RESOURCE_PROGRAM_CALLEE_RANGE, RESOURCE_PROGRAM_ENTRY_ARG_COUNT,
    RESOURCE_PROGRAM_ENTRY_DEPTH, RESOURCE_PROGRAM_EXT_UNKNOWN,
    RESOURCE_PROGRAM_EXT_UNSUPPORTED, RESOURCE_PROGRAM_EXT_ABI,
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