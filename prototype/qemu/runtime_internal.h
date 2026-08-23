/* Internal to this directory — not runtime_host.h's public API, just the
 * `Runtime` class and constants runtime_host.cpp and compile_proc.cpp
 * both need to agree on. Split out specifically so compile_proc.cpp can
 * be its own translation unit (qemu/Makefile's own comment on why:
 * `-Wstack-usage=`/`-Werror=stack-usage=` enforcement needs compileProc
 * isolated from anything — like the *_on_stack/_split variants' own
 * VLAs — whose stack usage is genuinely, deliberately unbounded at
 * compile time). */
#ifndef RUNTIME_INTERNAL_H
#define RUNTIME_INTERNAL_H

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include "runtime_host.h"

#define LANDING_TRAP 1u /* the boot record's tag half is 0 — enter_dispatch's own literal, qemu/runtime.S */
#define RESOURCE_ERROR_CODE 0x52455343u /* "RESC", arbitrary/distinct */

struct DispatchEntry { uint32_t code_ptr; uint32_t last_used; };

/* Defined in runtime_host.cpp (link-time const — see its own comment for
 * why no `| 1u` is needed). `extern` has to be repeated at the
 * definition too, unlike in C: a `const` global defaults to *internal*
 * linkage in C++, so without it neither compile_proc.cpp nor this
 * class's own methods below could see it at all — confirmed empirically
 * before converting anything, not just assumed. */
extern const uint32_t g_trampolineAddr;

/* The whole per-program-execution state, one object, allocated once by
 * whichever enter_program* variant is in play (runtime_host.cpp) and
 * reached everywhere else — compileProc, bailOut, enter_dispatch — via a
 * single pointer (r9) rather than a global. `dispatchTable` is a
 * flexible array member — GCC accepts the plain C99 spelling in C++ too,
 * verified empirically rather than assumed — indexed in its own,
 * natural, 0-based terms: `[0]` is the sentinel
 * (docs/design.md §2's info-block idea, made
 * literal here), `[1..procCount]` are the real per-procedure slots. The
 * ABI's own dispatchBase (r8) is `&dispatchTable[1]` — one past the
 * sentinel — so the asm side's 0-based procedure index `i` is always
 * this struct's own `dispatchTable[i + 1]`; every method below does that
 * `+1` exactly once, in `slot()`, rather than leaving each call site to
 * get it right on its own — the encapsulation this class exists for.
 *
 * A plain, trivial aggregate underneath the methods — public data, no
 * virtual functions, no base classes, no user-declared constructor —
 * deliberately: every caller still builds one the way the original C
 * version did, by reinterpreting a raw, `alignas`-aligned byte buffer (a
 * C-stack VLA or a static array) as a `Runtime*` and calling `init()` to
 * fill it in, never by constructing one normally. A non-trivial type
 * (virtual functions, a real constructor) wouldn't survive that cast —
 * methods alone don't affect standard-layout status, confirmed
 * empirically (offsetof below), but those other things would. */
class Runtime
{
public:
    uint32_t savedSp; /* written by enter_dispatch's own asm, never by C++ — see its own header */
    uint32_t arenaBase;
    uint32_t arenaEnd;
    uint32_t arenaCursor;
    const FlashProc *flashProcs;
    uint32_t procCount;
    DispatchEntry dispatchTable[];

private:
    /** Every dispatch-table `code_ptr` *write* goes through one of these
     *  two (design.md §16 item 7) — `BX`/`BLX` require bit 0 set to stay
     *  in Thumb mode on ARMv6-M; a value computed from raw arena
     *  arithmetic (`markCompiled`'s `dest`, `evict`'s slide) carries no
     *  such bit on its own, unlike a `.thumb_func`-tagged link-time symbol
     *  (`g_trampolineAddr`, already odd — runtime_host.cpp's own comment).
     *  Unconditionally OR-ing it in here, rather than at each call site
     *  reasoning case by case about which category a given value belongs
     *  to, makes every write safe by construction: idempotent on an
     *  already-tagged value, correct on a raw one, no third case to get
     *  wrong — and no future write site can simply forget to ask the
     *  question at all. */
    static void setCodePtr(DispatchEntry &e, uint32_t addr) { e.code_ptr = addr | 1u; }
    /** Slide an already-tagged `code_ptr` down by `delta` bytes
     *  (compaction, `evict`'s own survivor loop) — `delta` is always an
     *  even byte count, so the Thumb bit survives the subtraction
     *  unchanged in practice, but re-asserting it here costs nothing and
     *  means this call site isn't the one place trusting that invariant
     *  by hand either. */
    static void slideCodePtr(DispatchEntry &e, uint32_t delta) { e.code_ptr = (e.code_ptr - delta) | 1u; }

public:
    /** Bytes of storage one `Runtime` needs for `procCount` procedures —
     *  the fixed header plus one dispatch entry per procedure, plus the
     *  sentinel (index 0). Every caller sizes its own buffer with this
     *  instead of repeating the `sizeof`+FAM arithmetic by hand. */
    static uint32_t storageBytesFor(uint32_t procCount)
    {
        return (uint32_t)sizeof(Runtime) + (procCount + 1) * (uint32_t)sizeof(DispatchEntry);
    }

    /** Fills in every field except `savedSp` (enter_dispatch's own job)
     *  and points every dispatch slot at the translator trampoline —
     *  nothing is "compiled" yet. Doesn't allocate `codeArenaBase`'s own
     *  storage; where that lives is the caller's own choice (a C-stack
     *  VLA, caller-supplied memory, a static global — runtime_host.cpp's
     *  own enter_program* variants). */
    void init(uint32_t codeArenaBase, uint32_t codeArenaSize, const FlashProc *procs, uint32_t procCount_)
    {
        arenaBase = codeArenaBase;
        arenaEnd = codeArenaBase + codeArenaSize;
        arenaCursor = codeArenaBase;
        flashProcs = procs;
        procCount = procCount_;
        for(uint32_t i = 0; i < procCount; i++)
        {
            setCodePtr(dispatchTable[i + 1], g_trampolineAddr);
            dispatchTable[i + 1].last_used = 0;
        }
    }

    /** Procedure `idx`'s own dispatch slot — `[0]` is the sentinel, so
     *  every real procedure index is offset by one; every other method
     *  here goes through this rather than indexing `dispatchTable`
     *  directly, so nothing outside this class ever needs to know the
     *  `+1` exists at all. */
    DispatchEntry &slot(uint32_t idx) { return dispatchTable[idx + 1]; }
    const DispatchEntry &slot(uint32_t idx) const { return dispatchTable[idx + 1]; }

    /** The sentinel's own `code_ptr` — where a `RETURN` off the bottom of
     *  the whole excursion, or `bailOut`'s own direct jump, lands. */
    uint32_t sentinelLandingAddress() const { return dispatchTable[0].code_ptr; }

    bool isResident(uint32_t idx) const { return slot(idx).code_ptr != g_trampolineAddr; }

    bool hasRoomFor(uint32_t need) const { return arenaEnd - arenaCursor >= need; }

    /** Bump-allocates `need` bytes from the arena's own high-water mark,
     *  returning the destination address — the caller (compileProc)
     *  still does the actual copy; this only ever manages the
     *  bookkeeping, so it stays reusable once a real translator replaces
     *  the memcpy with genuine code generation. */
    uint32_t allocate(uint32_t need)
    {
        uint32_t dest = arenaCursor;
        arenaCursor += need;
        return dest;
    }

    /** Marks procedure `idx` resident at `dest`, freshly touched. */
    void markCompiled(uint32_t idx, uint32_t dest)
    {
        DispatchEntry &e = slot(idx);
        setCodePtr(e, dest);
        e.last_used = 0; /* the freshly-copied prologue stub bumps this on entry */
    }

    /** The globally-least-recently-used *resident* procedure, or -1 if
     *  none are resident at all (nothing pinned — docs' own §04/§08).
     *  `now` is the live LRU tick (r11) — this class has no opinion on
     *  where that value comes from, only on what to do with one once
     *  given it (compile_proc.cpp's own comment covers why it's read
     *  there). Age relative to `now`, unsigned not signed: `last_used`
     *  was always stamped at or before `now`, never after, so plain
     *  modular subtraction recovers the true elapsed tick count exactly,
     *  for any true gap up to just under 2^32. */
    int findEvictionVictim(uint32_t now) const
    {
        int victim = -1;
        uint32_t oldestAge = 0;
        for(uint32_t i = 0; i < procCount; i++)
        {
            if(!isResident(i)) continue;
            uint32_t age = now - slot(i).last_used;
            if(victim < 0 || age > oldestAge) { oldestAge = age; victim = (int)i; }
        }
        return victim;
    }

    /** How many bytes procedure `idx` currently occupies in the arena —
     *  not a stored field (docs/design.md §9): a
     *  real translator wouldn't have one for an already-compiled *other*
     *  procedure, only for whatever it's generating right now. Found the
     *  same way compaction has to anyway: scan for whichever other
     *  resident entry's code_ptr is the next-closest one above this
     *  one's, or the arena's current high-water mark if nothing sits
     *  above it. */
    uint32_t occupiedSizeOf(uint32_t idx) const
    {
        uint32_t addr = slot(idx).code_ptr & ~1u;
        uint32_t gapEnd = arenaCursor;
        for(uint32_t i = 0; i < procCount; i++)
        {
            if(!isResident(i)) continue;
            uint32_t a = slot(i).code_ptr & ~1u;
            if(a > addr && a < gapEnd) gapEnd = a;
        }
        return gapEnd - addr;
    }

    /** Evicts procedure `idx`: slides every resident procedure above it
     *  down by its own occupied size (memmove + fix up just the moved
     *  slots' own code_ptr — §11's position-independence, no other
     *  patching needed), frees its arena space, and marks it
     *  not-resident. */
    void evict(uint32_t idx)
    {
        uint32_t victimAddr = slot(idx).code_ptr & ~1u;
        uint32_t victimSize = occupiedSizeOf(idx);
        uint32_t gapEnd = victimAddr + victimSize;
        uint32_t tailLen = arenaCursor - gapEnd;

        memmove((void *)(uintptr_t)victimAddr, (void *)(uintptr_t)gapEnd, tailLen);
        arenaCursor -= victimSize;

        DispatchEntry &ve = slot(idx);
        setCodePtr(ve, g_trampolineAddr);
        ve.last_used = 0;

        for(uint32_t i = 0; i < procCount; i++)
        {
            DispatchEntry &e = slot(i);
            if(e.code_ptr != g_trampolineAddr && (e.code_ptr & ~1u) >= gapEnd)
                slideCodePtr(e, victimSize);
        }
    }
};

/* qemu/runtime.S hardcodes both of these (runtime_host.h) since it can't
 * call `offsetof` itself — these ties catch any layout drift at compile
 * time instead of letting it corrupt memory silently. Still valid on a
 * class with methods: standard-layout status (which offsetof needs)
 * turns on data-member/inheritance/virtual-function shape, not on
 * whether member functions exist. */
static_assert(offsetof(Runtime, dispatchTable) + sizeof(DispatchEntry) == RUNTIME_DISPATCH_TABLE_OFFSET,
    "runtime.S's own RUNTIME_DISPATCH_TABLE_OFFSET must match Runtime's real layout");
static_assert(sizeof(DispatchEntry) == DISPATCH_SENTINEL_OFFSET,
    "runtime.S's own DISPATCH_SENTINEL_OFFSET must match sizeof(DispatchEntry)");

#endif /* RUNTIME_INTERNAL_H */
