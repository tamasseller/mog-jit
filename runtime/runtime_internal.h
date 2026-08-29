/* Shared between enter_program.cpp, dispatch_abi.cpp, and compile_proc.cpp;
 * not part of runtime_host.h's public API. */
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

class Runtime;

extern "C" [[noreturn]] void runtimeBail(Runtime *runtime, uint32_t code);

struct ProcSlot
{
    uint32_t codePtr;    /* mutable — dispatch address (Thumb bit set) or trampolineAddr */
    uint32_t lastUsed;   /* mutable — LRU tick, stamped by runtime.S's callHelper/returnHelperTail */
    uint32_t bodyPtr;    /* static — absolute flash address of body_bytes (past this proc's own arg_count LEB128) */
    uint32_t staticInfo; /* static, packed: bit31 needsLRSave; bits[30:20] argCount; bits[19:0] bodyBytes */

    static constexpr uint32_t MAX_ARG_COUNT = (1u << 11) - 1;
    static constexpr uint32_t MAX_BODY_BYTES = (1u << 20) - 1;

    void setStaticInfo(uint32_t argCount, uint32_t bodyBytes, bool needsLRSave)
    {
        staticInfo = (needsLRSave ? 0x80000000u : 0u) | (argCount << 20) | bodyBytes;
    }

    uint32_t argCount() const
    {
        return (staticInfo >> 20) & MAX_ARG_COUNT;
    }

    uint32_t bodyBytes() const
    {
        return staticInfo & MAX_BODY_BYTES;
    }

    bool needsLRSave() const
    {
        return (staticInfo & 0x80000000u) != 0;
    }
};
static_assert(sizeof(ProcSlot) == 16, "power-of-two: idx*16 must stay a shift, not a multiply — runtime.S's own hardcoded stride");

extern const uint32_t trampolineAddr;

class Runtime
{
public:
    uint32_t savedSp; /* written by enterDispatch's own asm, never by C++ */
    uint32_t arenaEnd;
    uint32_t arenaCursor;
    uint32_t procCount;
    const ExtHooks *ext;
    uint32_t stackLimit;
    uint32_t arenaOverlapsStack; /* 0/1, not bool, so this struct's layout
                                  * (and RUNTIME_DISPATCH_TABLE_OFFSET/
                                  * DISPATCH_SENTINEL_OFFSET, runtime.S's own
                                  * hardcoded mirrors of it) never depends on
                                  * a compiler's bool size/alignment choice. */
    ProcSlot slots[];

private:
    static void setCodePtr(ProcSlot &entry, uint32_t addr)
    {
        entry.codePtr = addr | 1u;
    }

    static void slideCodePtr(ProcSlot &entry, uint32_t delta)
    {
        entry.codePtr = (entry.codePtr - delta) | 1u;
    }

public:
    static uint32_t storageBytesFor(uint32_t procCount)
    {
        return (uint32_t)sizeof(Runtime) + (procCount + 1) * (uint32_t)sizeof(ProcSlot);
    }

    uint32_t init(const uint8_t *programBytes, uint32_t programSize, uint32_t bodyOffset, uint32_t procCount,
        uint32_t codeArenaBase, uint32_t codeArenaSize, uint32_t stackLimit, uint32_t arenaOverlapsStack,
        const ExtHooks *extension = nullptr)
    {
        this->ext = extension;
        arenaEnd = (codeArenaBase + codeArenaSize) & ~3u;
        arenaCursor = (codeArenaBase + 3u) & ~3u;
        assert(arenaCursor <= arenaEnd); // GCOV_EXCL_LINE
        this->procCount = procCount;
        this->stackLimit = stackLimit;
        slots[0].lastUsed = 0;
        slots[0].bodyPtr = 0;
        slots[0].staticInfo = 0;
        this->arenaOverlapsStack = arenaOverlapsStack;

        if(extension != nullptr && extension->abiVersion != jitc::EXT_ABI_VERSION)
        {
            return RESOURCE_PROGRAM_EXT_ABI;
        }

        uint32_t pos = bodyOffset;
        for(uint32_t i = 0; i < procCount; i++)
        {
            assert(pos < programSize); // GCOV_EXCL_LINE — malformed/truncated program, matching decode_instr.cpp's own convention
            uint32_t argCount = jitc::decodeLeb128(programBytes, pos, pos);
            uint32_t bodyStart = pos;
            jitc::BodyScanResult scan = jitc::scanProcBody(programBytes, programSize, bodyStart, extension, stackLimit);
            if(!scan.ok)
            {
                return scan.failCode; // the walk already named which of its five rejections this is
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
            setCodePtr(s, trampolineAddr);
            s.lastUsed = 0;
            s.bodyPtr = (uint32_t)(uintptr_t)(programBytes + bodyStart);
            s.setStaticInfo(argCount, scan.bodyBytes, scan.needsLRSave);

            pos = bodyStart + scan.bodyBytes;
        }
        return 0;
    }

    /* The extension serving this program, or nullptr. */
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

    static uint32_t reserveFor(uint32_t need)
    {
        return (need + 3u) & ~3u;
    }

    uint32_t commit(uint32_t newEnd)
    {
        assert(arenaCursor <= newEnd && newEnd <= arenaEnd);
        
        arenaCursor = reserveFor(newEnd);
        
        return arenaCursor;
    }

    void markCompiled(uint32_t idx, uint32_t dest, uint32_t lruTick)
    {
        ProcSlot &entry = slot(idx);
        setCodePtr(entry, dest);
        entry.lastUsed = lruTick;
    }

    int findEvictionVictim(uint32_t now) const
    {
        int victim = -1;
        uint32_t oldestAge = 0;
        for(uint32_t i = 0; i < procCount; i++)
        {
            if(!isResident(i))
            {
                continue;
            }
            uint32_t age = now - slot(i).lastUsed;
            if(victim < 0 || age > oldestAge)
            {
                oldestAge = age;
                victim = (int)i;
            }
        }
        return victim;
    }

    uint32_t occupiedSizeOf(uint32_t idx) const
    {
        uint32_t addr = slot(idx).codePtr & ~1u;
        uint32_t gapEnd = arenaCursor;
        for(uint32_t i = 0; i < procCount; i++)
        {
            if(!isResident(i))
            {
                continue;
            }
            uint32_t a = slot(i).codePtr & ~1u;
            if(a > addr && a < gapEnd)
            {
                gapEnd = a;
            }
        }
        return gapEnd - addr;
    }

    void evict(uint32_t idx, const uint16_t *end)
    {
        uint32_t victimAddr = slot(idx).codePtr & ~1u;
        uint32_t victimSize = occupiedSizeOf(idx);
        uint32_t gapEnd = victimAddr + victimSize;
        uint32_t tailLen = (uint32_t)end - gapEnd;

        memmove((void *)(uintptr_t)victimAddr, (void *)(uintptr_t)gapEnd, tailLen);
        arenaCursor -= victimSize;

        ProcSlot &victimEntry = slot(idx);
        setCodePtr(victimEntry, trampolineAddr);
        victimEntry.lastUsed = 0;

        for(uint32_t i = 0; i < procCount; i++)
        {
            ProcSlot &entry = slot(i);
            if(entry.codePtr != trampolineAddr && (entry.codePtr & ~1u) >= gapEnd)
            {
                slideCodePtr(entry, victimSize);
            }
        }
    }

    uint16_t* ensureSpace(const uint16_t* end, uint32_t lruTick)
    {
        assert((uint32_t)end <= arenaEnd);

        if((uint32_t)end == arenaEnd)
        {
            int victim = findEvictionVictim(lruTick);
            if(victim < 0)
            {
                runtimeBail(this, RESOURCE_EXHAUSTED_ARENA);
                return nullptr;
            }


            evict((uint32_t)victim, end);
        }

        return (uint16_t *)(uintptr_t)arenaCursor;
    }
};

#if UINTPTR_MAX == 0xFFFFFFFFu
static_assert(offsetof(Runtime, slots) + sizeof(ProcSlot) == RUNTIME_DISPATCH_TABLE_OFFSET,
    "runtime.S's own RUNTIME_DISPATCH_TABLE_OFFSET must match Runtime's real layout");
static_assert(offsetof(Runtime, slots) + offsetof(ProcSlot, bodyPtr) == RUNTIME_EXT_STATE_OFFSET,
    "RUNTIME_EXT_STATE_OFFSET must be the sentinel slot's first unused word");
static_assert(RUNTIME_EXT_STATE_WORDS * 4 + offsetof(ProcSlot, bodyPtr) == sizeof(ProcSlot),
    "the extension scratch must be exactly the sentinel slot's unused tail");
#endif
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

#endif /* RUNTIME_INTERNAL_H */
