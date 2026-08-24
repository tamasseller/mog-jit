/* Shared between runtime_host.cpp and compile_proc_real.cpp; not part of
 * runtime_host.h's public API. */
#ifndef RUNTIME_INTERNAL_H
#define RUNTIME_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "runtime_host.h"

#define LANDING_TRAP 1u /* enterDispatch's boot-record tag for a trapped return; 0 means success */
#define RESOURCE_ERROR_CODE 0x52455343u /* "RESC", arbitrary/distinct */

struct DispatchEntry
{
    uint32_t codePtr;
    uint32_t lastUsed;
};

/* Link-time const, defined in runtime_host.cpp. `extern` has to be repeated
 * at the definition too: a `const` global defaults to internal linkage in
 * C++, so without it neither compileProc nor this class's own methods could
 * see it. */
extern const uint32_t trampolineAddr;

/* The whole per-execution state, allocated once by whichever enterProgram*
 * variant is in play and reached everywhere else through a single pointer
 * (r9). `dispatchTable` is a flexible array member, 0-based: `[0]` is the
 * sentinel (where a RETURN off the bottom of the excursion lands), and
 * `[1..procCount]` are the real per-procedure slots. The ABI's own
 * dispatchBase (r8) is `&dispatchTable[1]`, so every method here goes
 * through `slot()` to apply that `+1` exactly once rather than leaving each
 * call site to get it right independently.
 *
 * A plain, trivial aggregate deliberately: every caller builds one by
 * reinterpreting a raw, `alignas`-aligned byte buffer (a C-stack VLA or a
 * static array) as a `Runtime*` and calling `init()` to fill it in, never by
 * constructing one normally. A non-trivial type (virtual functions, a real
 * constructor) wouldn't survive that cast. */
class Runtime
{
public:
    uint32_t savedSp; /* written by enterDispatch's own asm, never by C++ */
    uint32_t arenaBase;
    uint32_t arenaEnd;
    uint32_t arenaCursor;
    const FlashProc *flashProcs;
    uint32_t procCount;
    /* The lowest address the translator's own LOOP/BR_TABLE recursion may
     * safely reach, checked live by translateProc's stackFloor parameter.
     * stackLimit is the hard, whole-execution floor. arenaOverlapsStack is
     * true only for enterProgramOnStack, whose code arena is anchored at
     * stackLimit itself and grows up from there — i.e. the same memory the
     * translator's own recursion runs on, unlike plain enterProgram's
     * separate static array or enterProgramSplit's caller-supplied,
     * unrelated memory. */
    uint32_t stackLimit;
    uint32_t arenaOverlapsStack; /* 0/1, not bool, so this struct's layout
                                  * (and RUNTIME_DISPATCH_TABLE_OFFSET/
                                  * DISPATCH_SENTINEL_OFFSET, runtime.S's own
                                  * hardcoded mirrors of it) never depends on
                                  * a compiler's bool size/alignment choice. */
    DispatchEntry dispatchTable[];

private:
    /* Every dispatch-table codePtr write goes through one of these two:
     * BX/BLX require bit 0 set to stay in Thumb mode on ARMv6-M, and a value
     * computed from raw arena arithmetic carries no such bit on its own,
     * unlike a .thumb_func-tagged link-time symbol (already odd). */
    static void setCodePtr(DispatchEntry &entry, uint32_t addr)
    {
        entry.codePtr = addr | 1u;
    }

    /* Slide an already-tagged codePtr down by delta bytes (compaction) —
     * delta is always even, so the Thumb bit survives the subtraction, but
     * re-asserting it here costs nothing. */
    static void slideCodePtr(DispatchEntry &entry, uint32_t delta)
    {
        entry.codePtr = (entry.codePtr - delta) | 1u;
    }

public:
    /* Bytes of storage one Runtime needs for procCount procedures: the
     * fixed header plus one dispatch entry per procedure, plus the
     * sentinel (index 0). */
    static uint32_t storageBytesFor(uint32_t procCount)
    {
        return (uint32_t)sizeof(Runtime) + (procCount + 1) * (uint32_t)sizeof(DispatchEntry);
    }

    /* Fills in every field except savedSp (enterDispatch's own job) and
     * points every dispatch slot at the translator trampoline — nothing is
     * compiled yet. Doesn't allocate codeArenaBase's own storage; where that
     * lives is the caller's choice. */
    void init(uint32_t codeArenaBase, uint32_t codeArenaSize, const FlashProc *procs, uint32_t procCount,
        uint32_t stackLimit, uint32_t arenaOverlapsStack)
    {
        arenaBase = codeArenaBase;
        arenaEnd = codeArenaBase + codeArenaSize;
        /* Rounded up here, not left to the caller: enterProgramOnStack
         * anchors the arena at stackLimit and enterProgramSplit takes the
         * base as a parameter, so neither can be covered by an alignas on
         * some array. Together with allocate()'s own rounding this makes
         * "arenaCursor is 4-aligned" inductive, which is what every
         * procedure's PC-relative literal loads depend on — see
         * allocate(). */
        arenaCursor = (codeArenaBase + 3u) & ~3u;
        flashProcs = procs;
        this->procCount = procCount;
        this->stackLimit = stackLimit;
        this->arenaOverlapsStack = arenaOverlapsStack;
        for(uint32_t i = 0; i < procCount; i++)
        {
            setCodePtr(dispatchTable[i + 1], trampolineAddr);
            dispatchTable[i + 1].lastUsed = 0;
        }
    }

    /* Procedure idx's own dispatch slot — [0] is the sentinel, so every real
     * procedure index is offset by one. */
    DispatchEntry &slot(uint32_t idx)
    {
        return dispatchTable[idx + 1];
    }

    const DispatchEntry &slot(uint32_t idx) const
    {
        return dispatchTable[idx + 1];
    }

    uint32_t sentinelLandingAddress() const
    {
        return dispatchTable[0].codePtr;
    }

    bool isResident(uint32_t idx) const
    {
        return slot(idx).codePtr != trampolineAddr;
    }

    bool hasRoomFor(uint32_t need) const
    {
        return arenaEnd - arenaCursor >= need;
    }

    /* The lowest address the translator's own recursion may reach right
     * now. Read fresh at the start of every compileProc call, never cached,
     * since arenaCursor moves between different procedures' compilations.
     * Only enterProgramOnStack's arena genuinely shares address space with
     * the stack; for the other two variants arenaCursor is meaningless
     * here, so it's excluded. */
    uint32_t liveStackFloor() const
    {
        return (arenaOverlapsStack && arenaCursor > stackLimit) ? arenaCursor : stackLimit;
    }

    /* What allocate(need) actually consumes — the value to check
     * hasRoomFor against, so eviction can't satisfy it on the unpadded
     * size and then have allocate() run past arenaEnd. */
    static uint32_t reserveFor(uint32_t need)
    {
        return (need + 3u) & ~3u;
    }

    /* Every procedure starts 4-aligned, and occupies a whole number of
     * words. Both halves matter for PC-relative literal loads: the
     * translator resolves LDR [pc,#imm] offsets in procedure-relative
     * terms, and those stay correct at runtime only because
     * Align(instrAddr + 4, 4) depends on instrAddr % 4 alone — which
     * equals the relative offset's own low bits exactly when the
     * procedure base is 4-aligned. Padding the reservation is what keeps
     * that true after eviction too: occupiedSizeOf is derived from
     * codePtr/arenaCursor gaps, so every compaction slide delta becomes a
     * multiple of 4 and no surviving procedure is ever knocked off
     * alignment by the memmove. */
    uint32_t allocate(uint32_t need)
    {
        uint32_t dest = arenaCursor;
        arenaCursor += reserveFor(need);
        return dest;
    }

    void markCompiled(uint32_t idx, uint32_t dest)
    {
        DispatchEntry &entry = slot(idx);
        setCodePtr(entry, dest);
        entry.lastUsed = 0; /* the freshly-copied prologue stub bumps this on entry */
    }

    /* The globally-least-recently-used resident procedure, or -1 if none are
     * resident. now is the live LRU tick (r11). Age relative to now,
     * unsigned not signed: lastUsed is always stamped at or before now, so
     * plain modular subtraction recovers the true elapsed tick count for any
     * true gap up to just under 2^32. */
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

    /* How many bytes procedure idx currently occupies in the arena — not a
     * stored field: scan for whichever other resident entry's codePtr is
     * the next-closest one above this one's, or the arena's current
     * high-water mark if nothing sits above it. */
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

    /* Evicts procedure idx: slides every resident procedure above it down by
     * its own occupied size (memmove + fix up just the moved slots' codePtr
     * — position-independent code needs no other patching), frees its arena
     * space, and marks it not-resident. */
    void evict(uint32_t idx)
    {
        uint32_t victimAddr = slot(idx).codePtr & ~1u;
        uint32_t victimSize = occupiedSizeOf(idx);
        uint32_t gapEnd = victimAddr + victimSize;
        uint32_t tailLen = arenaCursor - gapEnd;

        memmove((void *)(uintptr_t)victimAddr, (void *)(uintptr_t)gapEnd, tailLen);
        arenaCursor -= victimSize;

        DispatchEntry &victimEntry = slot(idx);
        setCodePtr(victimEntry, trampolineAddr);
        victimEntry.lastUsed = 0;

        for(uint32_t i = 0; i < procCount; i++)
        {
            DispatchEntry &entry = slot(i);
            if(entry.codePtr != trampolineAddr && (entry.codePtr & ~1u) >= gapEnd)
            {
                slideCodePtr(entry, victimSize);
            }
        }
    }
};

/* runtime.S hardcodes both of these since it can't call offsetof itself —
 * these ties catch any layout drift at compile time instead of letting it
 * corrupt memory silently.
 *
 * The table offset is checked only for a 32-bit target, the only thing
 * runtime.S is ever assembled for: flashProcs is a pointer, so on the
 * 64-bit host that builds the compiler's own unit tests the struct is
 * legitimately wider and the constant legitimately wouldn't match. */
#if UINTPTR_MAX == 0xFFFFFFFFu
static_assert(offsetof(Runtime, dispatchTable) + sizeof(DispatchEntry) == RUNTIME_DISPATCH_TABLE_OFFSET,
    "runtime.S's own RUNTIME_DISPATCH_TABLE_OFFSET must match Runtime's real layout");
#endif
static_assert(sizeof(DispatchEntry) == DISPATCH_SENTINEL_OFFSET,
    "runtime.S's own DISPATCH_SENTINEL_OFFSET must match sizeof(DispatchEntry)");

#endif /* RUNTIME_INTERNAL_H */
