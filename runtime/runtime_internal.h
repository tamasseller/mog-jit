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
#include "decode_instr.h"

#define LANDING_TRAP 1u /* enterDispatch's boot-record tag for a trapped return; 0 means success */
#define RESOURCE_ERROR_CODE 0x52455343u /* "RESC", arbitrary/distinct */

class Runtime;

/* An attached Assembler's own direct exit when it cannot free enough
 * arena room even after evicting everything resident (compiler/src/
 * assembler.h's fail()) — restores the caller's own saved sp and
 * transfers to the landing with code, never returning. The target
 * definition (runtime/compile_proc.cpp) is the old bailOut moved
 * verbatim: mov sp, savedSp; bx to the sentinel landing address, tagged
 * LANDING_TRAP. A host build has no such landing to jump to, so it
 * supplies its own longjmp-based definition instead (test/host's own
 * support file) — the same escape 1test's own CHECK() already unwinds
 * through, and the same pattern test_runtime_arena.cpp already uses to
 * satisfy trampolineAddr for a Runtime built outside the real runtime. */
extern "C" [[noreturn, weak]] void runtimeBail(Runtime *runtime, uint32_t code);

/* One procedure's whole entry: the runtime's own mutable dispatch state
 * (codePtr/lastUsed, exactly what runtime.S's hand-written asm touches)
 * plus the static facts a whole-program first pass (Runtime::init, below)
 * reads out of the wire bytes once and compileProc would otherwise have
 * to rediscover on every recompile. One combined array rather than two
 * parallel ones: a struct may have only one trailing flexible-array
 * member, and docs/design.md §2's own founding principle is that
 * everything lives in one contiguous region reachable through the fixed
 * ABI pointers, not a second ad hoc allocation. Stepped up from the
 * original DispatchEntry's 8 bytes to 16 to fit the static half in,
 * keeping the power-of-two addressing runtime.S depends on (idx*16 is
 * still a bare shift, not a multiply). */
struct ProcSlot
{
    uint32_t codePtr;    /* mutable — dispatch address (Thumb bit set) or trampolineAddr */
    uint32_t lastUsed;   /* mutable — LRU tick, bumped by the prologue stub */
    uint32_t bodyPtr;    /* static — absolute flash address of body_bytes (past this proc's own arg_count LEB128) */
    uint32_t staticInfo; /* static, packed: bit31 needsLRSave; bits[30:20] argCount; bits[19:0] bodyBytes */

    /* argCount/bodyBytes' own field widths — a procedure whose real value
     * doesn't fit either is rejected outright by Runtime::init() rather
     * than silently truncated. Not load-bearing anywhere else; revisit if
     * a real target's own programs need more headroom in either. */
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

/* Link-time const, defined in dispatch_abi.cpp. `extern` has to be repeated
 * at the definition too: a `const` global defaults to internal linkage in
 * C++, so without it neither compileProc nor this class's own methods could
 * see it. */
extern const uint32_t trampolineAddr;

/* The whole per-execution state, allocated once by whichever enterProgram*
 * variant is in play and reached everywhere else through a single pointer
 * (r9). `slots` is a flexible array member, 0-based: `[0]` is the
 * sentinel (where a RETURN off the bottom of the excursion lands), and
 * `[1..procCount]` are the real per-procedure slots. The ABI's own
 * dispatchBase (r8) is `&slots[1]`, so every method here goes through
 * `slot()` to apply that `+1` exactly once rather than leaving each call
 * site to get it right independently.
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
    uint32_t arenaEnd;
    uint32_t arenaCursor;
    uint32_t procCount;
    /* The lowest address the translator's own LOOP/BR_TABLE recursion may
     * safely reach, checked live by translateProc's stackFloor parameter.
     * stackLimit is the hard, whole-execution floor. arenaOverlapsStack is
     * true only for enterProgramOnStack, whose code arena is anchored at
     * stackLimit itself and grows up from there — i.e. the same memory the
     * translator's own recursion runs on, unlike enterProgramSplit's
     * caller-supplied, unrelated memory. */
    uint32_t stackLimit;
    uint32_t arenaOverlapsStack; /* 0/1, not bool, so this struct's layout
                                  * (and RUNTIME_DISPATCH_TABLE_OFFSET/
                                  * DISPATCH_SENTINEL_OFFSET, runtime.S's own
                                  * hardcoded mirrors of it) never depends on
                                  * a compiler's bool size/alignment choice. */
    ProcSlot slots[];

private:
    /* Every dispatch-table codePtr write goes through one of these two:
     * BX/BLX require bit 0 set to stay in Thumb mode on ARMv6-M, and a value
     * computed from raw arena arithmetic carries no such bit on its own,
     * unlike a .thumb_func-tagged link-time symbol (already odd). */
    static void setCodePtr(ProcSlot &entry, uint32_t addr)
    {
        entry.codePtr = addr | 1u;
    }

    /* Slide an already-tagged codePtr down by delta bytes (compaction) —
     * delta is always even, so the Thumb bit survives the subtraction, but
     * re-asserting it here costs nothing. */
    static void slideCodePtr(ProcSlot &entry, uint32_t delta)
    {
        entry.codePtr = (entry.codePtr - delta) | 1u;
    }

public:
    /* Bytes of storage one Runtime needs for procCount procedures: the
     * fixed header plus one slot per procedure, plus the sentinel
     * (index 0). */
    static uint32_t storageBytesFor(uint32_t procCount)
    {
        return (uint32_t)sizeof(Runtime) + (procCount + 1) * (uint32_t)sizeof(ProcSlot);
    }

    /* Fills in every field except savedSp (enterDispatch's own job),
     * points every dispatch slot at the translator trampoline, and — the
     * whole-program first pass docs/design.md §16 (and this session) found
     * missing entirely — walks the real wire bytes once to fill in every
     * slot's static half too: where its body lives, its own arg_count, and
     * whether it needs lr protected (jitc::scanProcBody, ported from
     * packages/machine/src/bytecode.ts's decodeProcBody). programBytes is
     * the whole serialized program (isa-core.md §5.5's proc_count +
     * procedures, past the jit-armv6m-specific max_call_depth/total_depth
     * envelope a caller's own parseProgramHeader already stripped);
     * bodyOffset is where procedure 0's own arg_count LEB128 begins.
     *
     * Returns false — never touching arenaCursor/dispatch state past
     * whichever procedure it failed on — if any procedure's own scan
     * overflows (scanProcBody's live stack-floor check) or doesn't fit
     * ProcSlot's packed field widths; the caller reports RESOURCE_ERROR
     * without ever reaching enterDispatch. Doesn't allocate codeArenaBase's
     * own storage; where that lives is the caller's choice. */
    bool init(const uint8_t *programBytes, uint32_t programSize, uint32_t bodyOffset, uint32_t procCount,
        uint32_t codeArenaBase, uint32_t codeArenaSize, uint32_t stackLimit, uint32_t arenaOverlapsStack)
    {
        arenaEnd = codeArenaBase + codeArenaSize;
        /* Rounded up here, not left to the caller: enterProgramOnStack
         * anchors the arena at stackLimit and enterProgramSplit takes the
         * base as a parameter, so neither can be covered by an alignas on
         * some array. Together with allocate()'s own rounding this makes
         * "arenaCursor is 4-aligned" inductive, which is what every
         * procedure's PC-relative literal loads depend on — see
         * allocate(). */
        arenaCursor = (codeArenaBase + 3u) & ~3u;
        this->procCount = procCount;
        this->stackLimit = stackLimit;
        this->arenaOverlapsStack = arenaOverlapsStack;

        uint32_t pos = bodyOffset;
        for(uint32_t i = 0; i < procCount; i++)
        {
            assert(pos < programSize); // GCOV_EXCL_LINE — malformed/truncated program, matching decode_instr.cpp's own convention
            uint32_t argCount = jitc::decodeLeb128(programBytes, pos, pos);
            uint32_t bodyStart = pos;
            jitc::BodyScanResult scan = jitc::scanProcBody(programBytes, programSize, bodyStart, stackLimit);
            if(!scan.ok || argCount > ProcSlot::MAX_ARG_COUNT || scan.bodyBytes > ProcSlot::MAX_BODY_BYTES)
            {
                return false;
            }

            ProcSlot &s = slot(i);
            setCodePtr(s, trampolineAddr);
            s.lastUsed = 0;
            s.bodyPtr = (uint32_t)(uintptr_t)(programBytes + bodyStart);
            s.setStaticInfo(argCount, scan.bodyBytes, scan.needsLRSave);

            pos = bodyStart + scan.bodyBytes;
        }
        return true;
    }

    /* Procedure idx's own slot — [0] is the sentinel, so every real
     * procedure index is offset by one. */
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
        ProcSlot &entry = slot(idx);
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

    /* Evicts procedure idx: slides every resident procedure above it, and
     * (docs/design.md §11's own "one compaction extension") whatever an
     * in-progress, not-yet-registered compilation has written above
     * arenaCursor too, down by the victim's own occupied size (memmove +
     * fix up just the moved slots' codePtr — position-independent code
     * needs no other patching), frees its arena space, and marks it
     * not-resident.
     *
     * inProgressLenBytes is the caller's own in-progress Assembler's
     * current halfwordCount()*2 — always 0 for a plain post-hoc eviction
     * (no in-progress translation in play), which is exactly compaction
     * as §8 first described it. The in-progress region's own base is
     * always exactly arenaCursor (nothing has bumped it — allocate() only
     * ever runs once, on success), so this single memmove, extended to
     * also cover it, keeps that invariant true on the other side: the
     * caller (compiler/src/assembler.cpp's growForAttached) rereads
     * arenaCursor afterward and rebases itself there. */
    void evict(uint32_t idx, uint32_t inProgressLenBytes = 0)
    {
        uint32_t victimAddr = slot(idx).codePtr & ~1u;
        uint32_t victimSize = occupiedSizeOf(idx);
        uint32_t gapEnd = victimAddr + victimSize;
        uint32_t tailLen = (arenaCursor + inProgressLenBytes) - gapEnd;

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
};

/* The other half of runtime_host.h's own split explanation, right above
 * its RUNTIME_DISPATCH_TABLE_OFFSET/DISPATCH_SENTINEL_OFFSET #defines:
 * runtime.S hardcodes both of those since it can't call offsetof itself,
 * and can't see this file's own Runtime/ProcSlot types at all (it
 * #includes runtime_host.h under __ASSEMBLER__, which hides this file
 * from it entirely) — so the two asserts below live here, next to the
 * real struct, checking the *other* file's numbers against it. Change
 * either side and this is what catches the drift at compile time instead
 * of letting it corrupt memory silently.
 *
 * The table offset is checked only for a 32-bit target, the only thing
 * runtime.S is ever assembled for: on the 64-bit host that builds the
 * compiler's own unit tests the struct is legitimately narrower by one
 * uint32_t than it would be with a pointer field, so nothing here depends
 * on pointer width to begin with any more (flashProcs is gone) — this
 * static_assert now holds on both widths, but is still guarded the same
 * way in case that ever changes again. */
#if UINTPTR_MAX == 0xFFFFFFFFu
static_assert(offsetof(Runtime, slots) + sizeof(ProcSlot) == RUNTIME_DISPATCH_TABLE_OFFSET,
    "runtime.S's own RUNTIME_DISPATCH_TABLE_OFFSET must match Runtime's real layout");
#endif
static_assert(sizeof(ProcSlot) == DISPATCH_SENTINEL_OFFSET,
    "runtime.S's own DISPATCH_SENTINEL_OFFSET must match sizeof(ProcSlot)");

#endif /* RUNTIME_INTERNAL_H */
